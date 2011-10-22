/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "../../stdafx.h"

#include "audio_decoder.h"
#include "audio_resampler.h"

#include "../util.h"
#include "../../ffmpeg_error.h"

#include <core/video_format.h>

#include <tbb/cache_aligned_allocator.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <connect.h>
#include <semaphore.h>

namespace caspar { namespace ffmpeg {
	
struct audio_decoder::implementation : boost::noncopyable
{	
	int															index_;
	std::shared_ptr<AVCodecContext>								codec_context_;		
	
	audio_resampler												resampler_;
	
	std::vector<int8_t,  tbb::cache_aligned_allocator<int8_t>>	buffer1_;

	Concurrency::transformer<packet_message_t, audio_message_t> transformer_;
	
public:
	explicit implementation(audio_decoder::source_t& source, audio_decoder::target_t& target, AVFormatContext& context, const core::video_format_desc& format_desc) 
		: codec_context_(open_codec(context, AVMEDIA_TYPE_AUDIO, index_))
		, resampler_(format_desc.audio_channels,	codec_context_->channels,
					 format_desc.audio_sample_rate, codec_context_->sample_rate,
					 AV_SAMPLE_FMT_S32,				codec_context_->sample_fmt)
		, buffer1_(AVCODEC_MAX_AUDIO_FRAME_SIZE*2)
		, transformer_(std::bind(&implementation::decode, this, std::placeholders::_1), &target, [this](const packet_message_t& message)
			{
				return message->payload && message->payload->stream_index == index_;
			})
	{			   	
		CASPAR_LOG(debug) << "[audio_decoder] " << context.streams[index_]->codec->codec->long_name;

		Concurrency::connect(source, transformer_);
	}

	audio_message_t decode(const packet_message_t& message)
	{		
		auto packet = message->payload;

		if(!packet)
			return make_message(std::shared_ptr<core::audio_buffer>());

		if(packet == loop_packet(index_))
			return make_message(loop_audio());

		if(packet == eof_packet(index_))
			return make_message(eof_audio());

		auto result = std::make_shared<core::audio_buffer>();

		while(packet->size > 0)
		{
			buffer1_.resize(AVCODEC_MAX_AUDIO_FRAME_SIZE*2);
			int written_bytes = buffer1_.size() - FF_INPUT_BUFFER_PADDING_SIZE;
		
			int ret = THROW_ON_ERROR2(avcodec_decode_audio3(codec_context_.get(), reinterpret_cast<int16_t*>(buffer1_.data()), &written_bytes, packet.get()), "[audio_decoder]");

			// There might be several frames in one packet.
			packet->size -= ret;
			packet->data += ret;
			
			buffer1_.resize(written_bytes);

			buffer1_ = resampler_.resample(std::move(buffer1_));
		
			const auto n_samples = buffer1_.size() / av_get_bytes_per_sample(AV_SAMPLE_FMT_S32);
			const auto samples = reinterpret_cast<int32_t*>(buffer1_.data());

			result->insert(result->end(), samples, samples + n_samples);
		}
				
		return make_message(result, message->token);
	}
};

audio_decoder::audio_decoder(audio_decoder::source_t& source, audio_decoder::target_t& target, AVFormatContext& context, const core::video_format_desc& format_desc)
	: impl_(new implementation(source, target, context, format_desc))
{
}

int64_t audio_decoder::nb_frames() const
{
	return 0;
}

}}