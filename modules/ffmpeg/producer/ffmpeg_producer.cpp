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

#include "../stdafx.h"

#include "ffmpeg_producer.h"

#include "../ffmpeg_error.h"

#include "muxer/frame_muxer.h"
#include "input/input.h"
#include "util/util.h"
#include "audio/audio_decoder.h"
#include "video/video_decoder.h"

#include <common/env.h>
#include <common/log.h>
#include <common/param.h>
#include <common/diagnostics/graph.h>

#include <core/video_format.h>
#include <core/producer/frame_producer.h>
#include <core/frame/frame_factory.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame_transform.h>
#include <core/monitor/monitor.h>

#include <boost/algorithm/string.hpp>
#include <common/assert.h>
#include <boost/assign.hpp>
#include <boost/timer.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>
#include <boost/thread/future.hpp>

#include <tbb/parallel_invoke.h>

#include <limits>
#include <memory>
#include <queue>

namespace caspar { namespace ffmpeg {
				
struct ffmpeg_producer : public core::frame_producer
{
	monitor::basic_subject										event_subject_;
	const std::wstring											filename_;
	
	const spl::shared_ptr<diagnostics::graph>					graph_;
					
	const spl::shared_ptr<core::frame_factory>					frame_factory_;
	const core::video_format_desc								format_desc_;

	input														input_;	
	std::unique_ptr<video_decoder>								video_decoder_;
	std::unique_ptr<audio_decoder>								audio_decoder_;	
	std::unique_ptr<frame_muxer>								muxer_;

	const double												fps_;
	const uint32_t												start_;
	const uint32_t												length_;
		
	int64_t														frame_number_;

	core::draw_frame							last_frame_;
	
public:
	explicit ffmpeg_producer(const spl::shared_ptr<core::frame_factory>& frame_factory, 
							 const core::video_format_desc& format_desc, 
							 const std::wstring& filename, 
							 const std::wstring& filter, 
							 bool loop, 
							 uint32_t start, 
							 uint32_t length) 
		: filename_(filename)
		, frame_factory_(frame_factory)		
		, format_desc_(format_desc)
		, input_(graph_, filename_, loop, start, length)
		, fps_(read_fps(*input_.context(), format_desc_.fps))
		, start_(start)
		, length_(length)
		, frame_number_(0)
		, last_frame_(core::draw_frame::empty())
	{
		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));	
		diagnostics::register_graph(graph_);
		
		try
		{
			video_decoder_.reset(new video_decoder(input_.context()));
			video_decoder_->subscribe(event_subject_);
			CASPAR_LOG(info) << print() << L" " << video_decoder_->print();
		}
		catch(averror_stream_not_found&)
		{
			//CASPAR_LOG(warning) << print() << " No video-stream found. Running without video.";	
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			CASPAR_LOG(warning) << print() << "Failed to open video-stream. Running without video.";	
		}

		try
		{
			audio_decoder_.reset(new audio_decoder(input_.context(), format_desc_));
			audio_decoder_->subscribe(event_subject_);
			CASPAR_LOG(info) << print() << L" " << audio_decoder_->print();
		}
		catch(averror_stream_not_found&)
		{
			//CASPAR_LOG(warning) << print() << " No audio-stream found. Running without audio.";	
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			CASPAR_LOG(warning) << print() << " Failed to open audio-stream. Running without audio.";		
		}	

		if(!video_decoder_ && !audio_decoder_)
			BOOST_THROW_EXCEPTION(averror_stream_not_found() << msg_info("No streams found"));

		muxer_.reset(new frame_muxer(fps_, frame_factory, format_desc_, filter));

		CASPAR_LOG(info) << print() << L" Initialized";
	}

	// frame_producer
	
	core::draw_frame receive(int flags) override
	{				
		boost::timer frame_timer;
				
		auto frame = core::draw_frame::late();		
		if(!try_decode_frame(frame, flags))
		{
			if(!input_.eof())		
				graph_->set_tag("underflow");	
		}
							
		graph_->set_value("frame-time", frame_timer.elapsed()*format_desc_.fps*0.5);
		event_subject_	<< monitor::event("profiler/time") % frame_timer.elapsed() % (1.0/format_desc_.fps);			
								
		graph_->set_text(print());

		if(frame != core::draw_frame::late())
		{
			++frame_number_;
			last_frame_ = frame;
		}
				
		event_subject_	<< monitor::event("file/time")			% monitor::duration(file_frame_number()/fps_) 
																% monitor::duration(file_nb_frames()/fps_)
						<< monitor::event("file/frame")			% static_cast<int32_t>(file_frame_number())
																% static_cast<int32_t>(file_nb_frames())
						<< monitor::event("file/fps")			% fps_
						<< monitor::event("file/path")			% filename_
						<< monitor::event("loop")				% input_.loop();

		return frame;
	}

	core::draw_frame last_frame() const override
	{
		return core::draw_frame::still(last_frame_);
	}
	
	uint32_t nb_frames() const override
	{
		if(input_.loop())
			return std::numeric_limits<uint32_t>::max();

		uint32_t nb_frames = file_nb_frames();

		nb_frames = std::min(length_, nb_frames);
		nb_frames = muxer_->calc_nb_frames(nb_frames);
		
		return nb_frames > start_ ? nb_frames - start_ : 0;
	}

	uint32_t file_nb_frames() const
	{
		uint32_t file_nb_frames = 0;
		file_nb_frames = std::max(file_nb_frames, video_decoder_ ? video_decoder_->nb_frames() : 0);
		file_nb_frames = std::max(file_nb_frames, audio_decoder_ ? audio_decoder_->nb_frames() : 0);
		return file_nb_frames;
	}

	uint32_t file_frame_number() const
	{
		return video_decoder_ ? video_decoder_->file_frame_number() : 0;
	}
	
	boost::unique_future<std::wstring> call(const std::wstring& param) override
	{
		boost::promise<std::wstring> promise;
		promise.set_value(do_call(param));
		return promise.get_future();
	}
				
	std::wstring print() const override
	{
		return L"ffmpeg[" + boost::filesystem::path(filename_).filename().wstring() + L"|" 
						  + print_mode() + L"|" 
						  + boost::lexical_cast<std::wstring>(file_frame_number()) + L"/" + boost::lexical_cast<std::wstring>(file_nb_frames()) + L"]";
	}

	std::wstring name() const override
	{
		return L"ffmpeg";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type",				L"ffmpeg");
		info.add(L"filename",			filename_);
		info.add(L"width",				video_decoder_ ? video_decoder_->width() : 0);
		info.add(L"height",				video_decoder_ ? video_decoder_->height() : 0);
		info.add(L"progressive",		video_decoder_ ? video_decoder_->is_progressive() : false);
		info.add(L"fps",				fps_);
		info.add(L"loop",				input_.loop());
		info.add(L"frame-number",		frame_number_);
		auto nb_frames2 = nb_frames();
		info.add(L"nb-frames",			nb_frames2 == std::numeric_limits<int64_t>::max() ? -1 : nb_frames2);
		info.add(L"file-frame-number",	file_frame_number());
		info.add(L"file-nb-frames",		file_nb_frames());
		return info;
	}
	
	void subscribe(const monitor::observable::observer_ptr& o) override
	{
		event_subject_.subscribe(o);
	}

	void unsubscribe(const monitor::observable::observer_ptr& o) override
	{
		event_subject_.unsubscribe(o);
	}

	// ffmpeg_producer

	std::wstring print_mode() const
	{
		return video_decoder_ ? ffmpeg::print_mode(video_decoder_->width(), video_decoder_->height(), fps_, !video_decoder_->is_progressive()) : L"n/a";
	}
					
	std::wstring do_call(const std::wstring& param)
	{
		static const boost::wregex loop_exp(L"LOOP\\s*(?<VALUE>\\d?)?", boost::regex::icase);
		static const boost::wregex seek_exp(L"SEEK\\s+(?<VALUE>\\d+)", boost::regex::icase);
		
		boost::wsmatch what;
		if(boost::regex_match(param, what, loop_exp))
		{
			if(!what["VALUE"].str().empty())
				input_.loop(boost::lexical_cast<bool>(what["VALUE"].str()));
			return boost::lexical_cast<std::wstring>(input_.loop());
		}
		if(boost::regex_match(param, what, seek_exp))
		{
			input_.seek(boost::lexical_cast<uint32_t>(what["VALUE"].str()));
			return L"";
		}

		BOOST_THROW_EXCEPTION(invalid_argument());
	}

	bool try_decode_frame(core::draw_frame& result, int flags)
	{
		for(int n = 0; n < 32; ++n)
		{
			if(muxer_->try_pop(result))			
				return true;			

			std::shared_ptr<AVPacket> pkt;

			for(int n = 0; n < 32 && ((video_decoder_ && !video_decoder_->ready()) || (audio_decoder_ && !audio_decoder_->ready())) && input_.try_pop(pkt); ++n)
			{
				if(video_decoder_)
					video_decoder_->push(pkt);
				if(audio_decoder_)
					audio_decoder_->push(pkt);
			}
		
			std::shared_ptr<AVFrame>			video;
			std::shared_ptr<core::audio_buffer> audio;

			tbb::parallel_invoke(
			[&]
			{
				if(!muxer_->video_ready() && video_decoder_)	
					video = video_decoder_->poll();	
			},
			[&]
			{		
				if(!muxer_->audio_ready() && audio_decoder_)		
					audio = audio_decoder_->poll();		
			});
		
			muxer_->push(video, flags);
			muxer_->push(audio);

			if(!audio_decoder_)
			{
				if(video == flush_video())
					muxer_->push(flush_audio());
				else if(!muxer_->audio_ready())
					muxer_->push(empty_audio());
			}

			if(!video_decoder_)
			{
				if(audio == flush_audio())
					muxer_->push(flush_video(), 0);
				else if(!muxer_->video_ready())
					muxer_->push(empty_video(), 0);
			}
		}

		return false;
	}
};

spl::shared_ptr<core::frame_producer> create_producer(const spl::shared_ptr<core::frame_factory>& frame_factory, const core::video_format_desc& format_desc, const std::vector<std::wstring>& params)
{		
	auto filename = probe_stem(env::media_folder() + L"\\" + params.at(0));

	if(filename.empty())
		return core::frame_producer::empty();
	
	auto loop		= boost::range::find(params, L"LOOP") != params.end();
	auto start		= get_param(L"SEEK", params, static_cast<uint32_t>(0));
	auto length		= get_param(L"LENGTH", params, std::numeric_limits<uint32_t>::max());
	auto filter_str = get_param(L"FILTER", params, L""); 	
		
	boost::replace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");
	boost::replace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
	
	return spl::make_shared_ptr(std::make_shared<ffmpeg_producer>(frame_factory, format_desc, filename, filter_str, loop, start, length));
}

}}