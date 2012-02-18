/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/
#include "../../stdafx.h"

#include "image_mixer.h"

#include "image_kernel.h"

#include "../util/device.h"
#include "../util/buffer.h"
#include "../util/texture.h"

#include <common/gl/gl_check.h>
#include <common/concurrency/async.h>
#include <common/memory/array.h>

#include <core/frame/frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <asmlib.h>

#include <gl/glew.h>

#include <boost/foreach.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/thread/future.hpp>

#include <algorithm>
#include <vector>

using namespace boost::assign;

namespace caspar { namespace accelerator { namespace ogl {
		
typedef boost::shared_future<spl::shared_ptr<texture>> future_texture;

struct item
{
	core::pixel_format_desc								pix_desc;
	core::field_mode									field_mode;
	std::vector<future_texture>							textures;
	core::image_transform								transform;

	item()
		: pix_desc(core::pixel_format::invalid)
		, field_mode(core::field_mode::empty)
	{
	}
};

struct layer
{
	std::vector<item>	items;
	core::blend_mode	blend_mode;

	layer()
		: blend_mode(core::blend_mode::normal)
	{
	}

	layer(std::vector<item> items, core::blend_mode blend_mode)
		: items(std::move(items))
		, blend_mode(blend_mode)
	{
	}
};

class image_renderer
{
	spl::shared_ptr<device>	ogl_;
	image_kernel			kernel_;
public:
	image_renderer(const spl::shared_ptr<device>& ogl)
		: ogl_(ogl)
		, kernel_(ogl_)
	{
	}
	
	boost::unique_future<core::const_array> operator()(std::vector<layer> layers, const core::video_format_desc& format_desc)
	{	
		if(layers.empty())
		{ // Bypass GPU with empty frame.
			auto buffer = spl::make_shared<const std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>>(format_desc.size, 0);
			return async(launch::deferred, [=]
			{
				return core::const_array(buffer->data(), format_desc.size, true, buffer);
			});
		}		

		return flatten(ogl_->begin_invoke([=]() mutable -> boost::shared_future<core::const_array>
		{
			auto draw_buffer = create_mixer_buffer(format_desc.width, format_desc.height, 4);

			if(format_desc.field_mode != core::field_mode::progressive)
			{
				draw(layers,			draw_buffer, format_desc, core::field_mode::upper);
				draw(std::move(layers), draw_buffer, format_desc, core::field_mode::lower);
			}
			else			
				draw(std::move(layers), draw_buffer, format_desc, core::field_mode::progressive);
								
			return make_shared(ogl_->copy_async(draw_buffer));
		}));
	}

private:	
	
	void draw(std::vector<layer>				layers, 
			  spl::shared_ptr<texture>&			draw_buffer, 
			  const core::video_format_desc&	format_desc,
			  core::field_mode					field_mode)
	{
		std::shared_ptr<texture> layer_key_buffer;

		BOOST_FOREACH(auto& layer, layers)
			draw_layer(std::move(layer), draw_buffer, layer_key_buffer, format_desc, field_mode);
	}

	void draw_layer(layer							layer, 
					spl::shared_ptr<texture>&		draw_buffer,
					std::shared_ptr<texture>&		layer_key_buffer,
					const core::video_format_desc&	format_desc,
					core::field_mode				field_mode)
	{		
		// Fix frames		
		BOOST_FOREACH(auto& item, layer.items)		
		{
			if(item.pix_desc.planes.at(0).height == 480) // NTSC DV
			{
				item.transform.fill_translation[1] += 2.0/static_cast<double>(format_desc.height);
				item.transform.fill_scale[1] = 1.0 - 6.0*1.0/static_cast<double>(format_desc.height);
			}
	
			// Fix field-order if needed
			if(item.field_mode == core::field_mode::lower && format_desc.field_mode == core::field_mode::upper)
				item.transform.fill_translation[1] += 1.0/static_cast<double>(format_desc.height);
			else if(item.field_mode == core::field_mode::upper && format_desc.field_mode == core::field_mode::lower)
				item.transform.fill_translation[1] -= 1.0/static_cast<double>(format_desc.height);
		}

		// Mask out fields
		BOOST_FOREACH(auto& item, layer.items)				
			item.transform.field_mode &= field_mode;
		
		// Remove empty items.
		boost::range::remove_erase_if(layer.items, [&](const item& item)
		{
			return item.transform.field_mode == core::field_mode::empty;
		});
		
		// Remove first field stills.
		boost::range::remove_erase_if(layer.items, [&](const item& item)
		{
			return item.transform.is_still && item.transform.field_mode == format_desc.field_mode; // only use last field for stills.
		});

		if(layer.items.empty())
			return;

		std::shared_ptr<texture> local_key_buffer;
		std::shared_ptr<texture> local_mix_buffer;
				
		if(layer.blend_mode != core::blend_mode::normal)
		{
			auto layer_draw_buffer = create_mixer_buffer(draw_buffer->width(), draw_buffer->height(), 4);

			BOOST_FOREACH(auto& item, layer.items)
				draw_item(std::move(item), layer_draw_buffer, layer_key_buffer, local_key_buffer, local_mix_buffer);	
		
			draw_mixer_buffer(layer_draw_buffer, std::move(local_mix_buffer), core::blend_mode::normal);							
			draw_mixer_buffer(draw_buffer, std::move(layer_draw_buffer), layer.blend_mode);
		}
		else // fast path
		{
			BOOST_FOREACH(auto& item, layer.items)		
				draw_item(std::move(item), draw_buffer, layer_key_buffer, local_key_buffer, local_mix_buffer);		
					
			draw_mixer_buffer(draw_buffer, std::move(local_mix_buffer), core::blend_mode::normal);
		}					

		layer_key_buffer = std::move(local_key_buffer);
	}

	void draw_item(item							item, 
				   spl::shared_ptr<texture>&	draw_buffer, 
				   std::shared_ptr<texture>&	layer_key_buffer, 
				   std::shared_ptr<texture>&	local_key_buffer, 
				   std::shared_ptr<texture>&	local_mix_buffer)
	{					
		draw_params draw_params;
		draw_params.pix_desc	= std::move(item.pix_desc);
		draw_params.transform	= std::move(item.transform);
		BOOST_FOREACH(auto& future_texture, item.textures)
			draw_params.textures.push_back(future_texture.get());

		if(item.transform.is_key)
		{
			local_key_buffer = local_key_buffer ? local_key_buffer : create_mixer_buffer(draw_buffer->width(), draw_buffer->height(), 1);

			draw_params.background			= local_key_buffer;
			draw_params.local_key			= nullptr;
			draw_params.layer_key			= nullptr;

			kernel_.draw(std::move(draw_params));
		}
		else if(item.transform.is_mix)
		{
			local_mix_buffer = local_mix_buffer ? local_mix_buffer : create_mixer_buffer(draw_buffer->width(), draw_buffer->height(), 4);

			draw_params.background			= local_mix_buffer;
			draw_params.local_key			= std::move(local_key_buffer);
			draw_params.layer_key			= layer_key_buffer;

			draw_params.keyer				= keyer::additive;

			kernel_.draw(std::move(draw_params));
		}
		else
		{
			draw_mixer_buffer(draw_buffer, std::move(local_mix_buffer), core::blend_mode::normal);
			
			draw_params.background			= draw_buffer;
			draw_params.local_key			= std::move(local_key_buffer);
			draw_params.layer_key			= layer_key_buffer;

			kernel_.draw(std::move(draw_params));
		}	
	}

	void draw_mixer_buffer(spl::shared_ptr<texture>&	draw_buffer, 
						   std::shared_ptr<texture>&&	source_buffer, 
						   core::blend_mode				blend_mode = core::blend_mode::normal)
	{
		if(!source_buffer)
			return;

		draw_params draw_params;
		draw_params.pix_desc.format		= core::pixel_format::bgra;
		draw_params.pix_desc.planes		= list_of(core::pixel_format_desc::plane(source_buffer->width(), source_buffer->height(), 4));
		draw_params.textures			= list_of(source_buffer);
		draw_params.transform			= core::image_transform();
		draw_params.blend_mode			= blend_mode;
		draw_params.background			= draw_buffer;

		kernel_.draw(std::move(draw_params));
	}
			
	spl::shared_ptr<texture> create_mixer_buffer(int width, int height, int stride)
	{
		auto buffer = ogl_->create_texture(width, height, stride);
		buffer->clear();
		return buffer;
	}
};
		
struct image_mixer::impl : boost::noncopyable
{	
	spl::shared_ptr<device>				ogl_;
	image_renderer						renderer_;
	std::vector<core::image_transform>	transform_stack_;
	std::vector<layer>					layers_; // layer/stream/items
public:
	impl(const spl::shared_ptr<device>& ogl) 
		: ogl_(ogl)
		, renderer_(ogl)
		, transform_stack_(1)	
	{
		CASPAR_LOG(info) << L"Initialized OpenGL Accelerated GPU Image Mixer";
	}

	void begin_layer(core::blend_mode blend_mode)
	{
		layers_.push_back(layer(std::vector<item>(), blend_mode));
	}
		
	void push(const core::frame_transform& transform)
	{
		transform_stack_.push_back(transform_stack_.back()*transform.image_transform);
	}
		
	void visit(const core::const_frame& frame)
	{			
		if(frame.pixel_format_desc().format == core::pixel_format::invalid)
			return;

		if(frame.pixel_format_desc().planes.empty())
			return;

		if(transform_stack_.back().field_mode == core::field_mode::empty)
			return;

		item item;
		item.pix_desc	= frame.pixel_format_desc();
		item.field_mode	= frame.field_mode();
		item.transform	= transform_stack_.back();
		for(int n = 0; n < static_cast<int>(item.pix_desc.planes.size()); ++n)
			item.textures.push_back(ogl_->copy_async(frame.image_data(n), item.pix_desc.planes[n].width, item.pix_desc.planes[n].height, item.pix_desc.planes[n].stride));
		
		layers_.back().items.push_back(item);
	}

	void pop()
	{
		transform_stack_.pop_back();
	}

	void end_layer()
	{		
	}
	
	boost::unique_future<core::const_array> render(const core::video_format_desc& format_desc)
	{
		return renderer_(std::move(layers_), format_desc);
	}
	
	virtual core::mutable_frame create_frame(const void* tag, const core::pixel_format_desc& desc, double frame_rate, core::field_mode field_mode)
	{
		std::vector<core::mutable_array> buffers;
		BOOST_FOREACH(auto& plane, desc.planes)		
			buffers.push_back(ogl_->create_array(plane.size));		

		return core::mutable_frame(std::move(buffers), core::audio_buffer(), tag, desc, frame_rate, field_mode);
	}
};

image_mixer::image_mixer(const spl::shared_ptr<device>& ogl) : impl_(new impl(ogl)){}
image_mixer::~image_mixer(){}
void image_mixer::push(const core::frame_transform& transform){impl_->push(transform);}
void image_mixer::visit(const core::const_frame& frame){impl_->visit(frame);}
void image_mixer::pop(){impl_->pop();}
boost::unique_future<core::const_array> image_mixer::operator()(const core::video_format_desc& format_desc){return impl_->render(format_desc);}
void image_mixer::begin_layer(core::blend_mode blend_mode){impl_->begin_layer(blend_mode);}
void image_mixer::end_layer(){impl_->end_layer();}
core::mutable_frame image_mixer::create_frame(const void* tag, const core::pixel_format_desc& desc, double frame_rate, core::field_mode field_mode) {return impl_->create_frame(tag, desc, frame_rate, field_mode);}

}}}