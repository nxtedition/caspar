#pragma once

#include <memory>
#include <array>

namespace caspar { namespace core {
	
struct pixel_format
{
	enum type
	{
		bgra,
		rgba,
		argb,
		abgr,
		ycbcr,
		ycbcra,
		count,
		invalid
	};
};

struct pixel_format_desc
{
	struct plane
	{
		plane() : linesize(0), width(0), height(0), size(0), channels(0){}
		plane(size_t width, size_t height, size_t channels)
			: linesize(width*channels), width(width), height(height), size(width*height*channels), channels(channels)
		{}
		size_t linesize;
		size_t width;
		size_t height;
		size_t size;
		size_t channels;
	};

	pixel_format_desc() : pix_fmt(pixel_format::invalid)
	{
		std::fill(planes.begin(), planes.end(), plane());
	}
	
	pixel_format::type pix_fmt;
	std::array<plane, 4> planes;
};

static size_t hash(const pixel_format_desc& desc)
{
	size_t hash = 0;
	switch(desc.pix_fmt)
	{
	case pixel_format::ycbcr:
	case pixel_format::ycbcra:
		//  0-10 (11) width
		// 11-21 (11) height
		// 22-24 (3)  x-ratio
		// 25-27 (3)  y-ratio
		// 28-29 (2)  unused
		// 30	 (1)  alpha
		// 31    (1)  yuv = true => 1
		hash |= ( desc.planes[0].width							& 0x7FF	) << 0;
		hash |= ( desc.planes[0].height							& 0x7FF	) << 11;
		hash |= ((desc.planes[0].height/desc.planes[1].height)	& 0x7	) << 22;
		hash |= ((desc.planes[0].width/desc.planes[1].width)	& 0x7	) << 25;
		hash |= desc.pix_fmt == pixel_format::ycbcra ? (1 << 30) : 0;
		hash |= 1 << 31;
		return hash;
	case pixel_format::bgra:
	case pixel_format::rgba:
	case pixel_format::argb:
	case pixel_format::abgr:
		
		//0-10  (11) height
		//11-21 (11) width
		//22-29 (8)  unused
		//30	(1)  alpha
		//31	(1)  yuv = false => 0
		hash |= (desc.planes[0].height & 0xFFFF) << 0;
		hash |= (desc.planes[0].width  & 0xFFFF) << 15;
		hash |= 1 << 30;
		return hash;

	default:
		return hash;
	};
}
	
class pixel_format_desc_hash
{
public:
	size_t operator()(const pixel_format_desc& desc) const
	{
		return hash(desc);
	}
};

inline bool operator==(const pixel_format_desc& lhs, const pixel_format_desc& rhs)
{
	return hash(lhs) == hash(rhs);
}

inline bool operator!=(const pixel_format_desc& lhs, const pixel_format_desc& rhs)
{
	return !(lhs == rhs);
}
	
}}