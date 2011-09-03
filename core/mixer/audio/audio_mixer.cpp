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

#include "audio_mixer.h"

#include <core/mixer/write_frame.h>
#include <core/producer/frame/frame_transform.h>

#include <tbb/parallel_for.h>

#include <safeint.h>

#include <stack>
#include <deque>

namespace caspar { namespace core {

struct audio_item
{
	const void*			tag;
	frame_transform		transform;
	audio_buffer		audio_data;
};
	
struct audio_mixer::implementation
{
	std::stack<core::frame_transform>				transform_stack_;
	std::map<const void*, core::frame_transform>	prev_frame_transforms_;
	const core::video_format_desc					format_desc_;
	std::vector<audio_item>							items;

public:
	implementation(const core::video_format_desc& format_desc)
		: format_desc_(format_desc)
	{
		transform_stack_.push(core::frame_transform());
	}
	
	void begin(core::basic_frame& frame)
	{
		transform_stack_.push(transform_stack_.top()*frame.get_frame_transform());
	}

	void visit(core::write_frame& frame)
	{
		// We only care about the last field.
		if(format_desc_.field_mode == field_mode::upper && transform_stack_.top().field_mode == field_mode::upper)
			return;

		if(format_desc_.field_mode == field_mode::lower && transform_stack_.top().field_mode == field_mode::lower)
			return;

		// Skip empty audio.
		if(transform_stack_.top().volume < 0.002 || frame.audio_data().empty())
			return;

		audio_item item;
		item.tag		= frame.tag();
		item.transform	= transform_stack_.top();
		item.audio_data = std::move(frame.audio_data());

		items.push_back(item);		
	}

	void begin(const core::frame_transform& transform)
	{
		transform_stack_.push(transform_stack_.top()*transform);
	}
		
	void end()
	{
		transform_stack_.pop();
	}
	
	audio_buffer mix()
	{	
		auto intermediate = std::vector<float, tbb::cache_aligned_allocator<float>>(format_desc_.audio_samples_per_frame+128, 0.0f);

		std::map<const void*, core::frame_transform> next_frame_transforms;
		
		tbb::affinity_partitioner ap;

		BOOST_FOREACH(auto& item, items)
		{				
			const auto next = item.transform;
			auto prev = next;

			const auto it = prev_frame_transforms_.find(item.tag);
			if(it != prev_frame_transforms_.end())
				prev = it->second;
				
			next_frame_transforms[item.tag] = next; // Store all active tags, inactive tags will be removed at the end.
				
			if(next.volume < 0.001 && prev.volume < 0.001)
				continue;
									
			if(static_cast<size_t>(item.audio_data.size()) != format_desc_.audio_samples_per_frame)
				continue;

			CASPAR_ASSERT(format_desc_.audio_channels == 2);
			CASPAR_ASSERT(format_desc_.audio_samples_per_frame % 4 == 0);
						
			const float prev_volume = static_cast<float>(prev.volume);
			const float next_volume = static_cast<float>(next.volume);
			const float delta		= 1.0f/static_cast<float>(format_desc_.audio_samples_per_frame/format_desc_.audio_channels);
			
			tbb::parallel_for
			(
				tbb::blocked_range<size_t>(0, format_desc_.audio_samples_per_frame/4),
				[&](const tbb::blocked_range<size_t>& r)
				{					
					auto alpha_ps	= _mm_setr_ps(delta, delta, 0.0f, 0.0f);
					auto delta2_ps	= _mm_set_ps1(delta*2.0f);
					auto prev_ps	= _mm_set_ps1(prev_volume);
					auto next_ps	= _mm_set_ps1(next_volume);	

					for(size_t n = r.begin(); n < r.end(); ++n)
					{		
						auto next2_ps		= _mm_mul_ps(next_ps, alpha_ps);
						auto prev2_ps		= _mm_sub_ps(prev_ps, _mm_mul_ps(prev_ps, alpha_ps));
						auto volume_ps		= _mm_add_ps(next2_ps, prev2_ps);

						auto sample_ps		= _mm_cvtepi32_ps(_mm_load_si128(reinterpret_cast<__m128i*>(&item.audio_data[n*4])));
						auto res_sample_ps	= _mm_load_ps(&intermediate[n*4]);											
						sample_ps			= _mm_mul_ps(sample_ps, volume_ps);	
						res_sample_ps		= _mm_add_ps(sample_ps, res_sample_ps);	

						alpha_ps			= _mm_add_ps(alpha_ps, delta2_ps);

						_mm_store_ps(&intermediate[n*4], res_sample_ps);
					}
				}
			, ap);
		}
		
		auto result = audio_buffer(format_desc_.audio_samples_per_frame+128, 0);	
		
		tbb::parallel_for
		(
			tbb::blocked_range<size_t>(0, format_desc_.audio_samples_per_frame/32),
			[&](const tbb::blocked_range<size_t>& r)
			{		
				auto intermediate_128 = reinterpret_cast<__m128i*>(intermediate.data()+r.begin()*32);
				auto result_128		  = reinterpret_cast<__m128i*>(result.data()+r.begin()*32);
				
				for(size_t n = r.begin(); n < r.end(); ++n)
				{	
					auto xmm0 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm1 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm2 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm3 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm4 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm5 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm6 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
					auto xmm7 = _mm_load_ps(reinterpret_cast<float*>(intermediate_128++));
			
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm0));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm1));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm2));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm3));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm4));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm5));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm6));
					_mm_stream_si128(result_128++, _mm_cvtps_epi32(xmm7));
				}
			}
		, ap);

		items.clear();
		prev_frame_transforms_ = std::move(next_frame_transforms);	

		result.resize(format_desc_.audio_samples_per_frame);
		return std::move(result);
	}
};

audio_mixer::audio_mixer(const core::video_format_desc& format_desc) : impl_(new implementation(format_desc)){}
void audio_mixer::begin(core::basic_frame& frame){impl_->begin(frame);}
void audio_mixer::visit(core::write_frame& frame){impl_->visit(frame);}
void audio_mixer::end(){impl_->end();}
audio_buffer audio_mixer::mix(){return impl_->mix();}
audio_mixer& audio_mixer::operator=(audio_mixer&& other)
{
	impl_ = std::move(other.impl_);
	return *this;
}

}}