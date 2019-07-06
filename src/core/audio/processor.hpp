/// \file
/// Audio Processors are anything that can process audio/midi.
/// They run on the audio thread, and are called by the audio system (Jack).
/// Formally, an audio processor is defined as having a method matching
/// this signature:
///
/// ```cpp
/// void process(const ProcessData&);
/// ```
///
/// This method _must_ not be called from anywhere other than the main
/// audio system and its delegates.
///
/// If another thread needs access to any of this data, e.g. the audio/midi
/// data, they need an audio processor to read it and store it. It is up to the
/// engine in question to handle thread safety.

#pragma once

#include <exception>
#include <functional>
#include <gsl/span>

#include "core/audio/midi.hpp"

#include "util/audio.hpp"

namespace otto::core::audio {

  /**
   * Checks if a type qualifies as an <AudioProcessor>
   */
  template<typename...>
  struct is_audio_processor {}; // TODO: Implementation

  template<typename T>
  struct audio_frame_channels;

  template<std::size_t N>
  struct audio_frame_channels<std::array<float, N>> {
    static constexpr auto value = N;
  };

  /// A handle to an audio buffer
  struct AudioBufferHandle {
    using iterator = float*;
    using pointer = float*;
    using const_iterator = const float*;

    AudioBufferHandle(float* data, std::size_t length, int& reference_count) noexcept
      : _data(data), _length(length), _reference_count(&reference_count)
    {
      (*_reference_count)++;
    }

    ~AudioBufferHandle() noexcept
    {
      if (_reference_count) (*_reference_count)--;
    };

    AudioBufferHandle(AudioBufferHandle&& rhs) noexcept
      : _data(rhs._data), _length(rhs._length), _reference_count(rhs._reference_count)
    {
      rhs._data = nullptr;
      rhs._reference_count = nullptr;
    }

    AudioBufferHandle(const AudioBufferHandle& rhs) noexcept
      : _data(rhs._data), _length(rhs._length), _reference_count(rhs._reference_count)
    {
      (*_reference_count)++;
    }

    AudioBufferHandle& operator=(AudioBufferHandle&& rhs) noexcept
    {
      _data = rhs._data;
      _length = rhs._length;
      _reference_count = rhs._reference_count;
      rhs._data = nullptr;
      rhs._reference_count = nullptr;
      return *this;
    }

    AudioBufferHandle& operator=(const AudioBufferHandle& rhs) noexcept
    {
      _data = rhs._data;
      _length = rhs._length;
      _reference_count = rhs._reference_count;
      (*_reference_count)++;
      return *this;
    }

    int reference_count() const
    {
      return *_reference_count;
    }

    float* data() const
    {
      return _data;
    }

    std::size_t size() const
    {
      return _length;
    }

    iterator begin()
    {
      return _data;
    }
    iterator end()
    {
      return _data + _length;
    }
    iterator begin() const
    {
      return _data;
    }
    iterator end() const
    {
      return _data + _length;
    }

    float& operator[](std::size_t i)
    {
      return _data[i];
    }

    const float& operator[](std::size_t i) const
    {
      return _data[i];
    }

    void release()
    {
      (*_reference_count)--;
      _reference_count = nullptr;
      _data = nullptr;
    }

    void clear()
    {
      std::fill(begin(), end(), 0);
    }

    /// Get only a slice of the audio.
    ///
    /// \param idx The index to start from
    /// \param length The number of frames to keep in the slice
    ///   If `length` is negative, `nframes - idx` will be used
    /// \requires parameter `idx` shall be in the range `[0, _length)`, and
    /// `length` shall be in range `[0, nframes - idx]`
    AudioBufferHandle slice(int idx, int length = -1)
    {
      length = length < 0 ? _length - idx : length;
      return {_data + idx, std::size_t(length), *_reference_count};
    }

    float* data()
    {
      return _data;
    }

    operator std::array<AudioBufferHandle, 1>() const noexcept
    {
      return {*this};
    }

  private:
    float* _data;
    std::size_t _length;
    int* _reference_count;
  };

  struct AudioBufferPool {
    static constexpr int number_of_buffers = 8;
    AudioBufferPool(std::size_t buffer_size) : buffer_size(buffer_size)
    {
      // For now this is hardcoded, which is nice, cause we notice if we suddenly are using too many
      // buffers
      reserve(number_of_buffers);
    }

    AudioBufferHandle allocate() noexcept
    {
      for (std::size_t i = 0; i < reference_counts.size(); i++) {
        if (reference_counts[i] < 1) {
          if (i > _max_val) {
            LOGI("Using {} buffers", i + 1);
            _max_val = i;
          }
          reference_counts[i] = 0;
          int index = i * buffer_size;
          return {data.get() + index, buffer_size, reference_counts[i]};
        }
      }
      // TODO: handle this reasonably
      LOGF("No free audio buffers found. This shouldn't happen");
      std::terminate();
    }

    AudioBufferHandle allocate_clear()
    {
      auto res = allocate();
      res.clear();
      return res;
    }

    template<int NN>
    std::array<AudioBufferHandle, NN> allocate_multi() noexcept
    {
      return util::generate_array<NN>([this](int) { return allocate(); });
    }

    template<int NN>
    std::array<AudioBufferHandle, NN> allocate_multi_clear() noexcept
    {
      return util::generate_array<NN>([this](int) { return allocate_clear(); });
    }

    void set_buffer_size(std::size_t bs) noexcept
    {
      buffer_size = bs;
      reserve(number_of_buffers);
    }

  private:
    void reserve(std::size_t n) noexcept
    {
      data = std::make_unique<float[]>(n * buffer_size);
      _avaliable_buffers = n;
      reference_counts.resize(_avaliable_buffers, 0);
    }

    std::size_t buffer_size;
    std::vector<int> reference_counts;
    std::size_t _avaliable_buffers = 0;
    std::unique_ptr<float[]> data;
    std::size_t _max_val = -1;
  };

  /// Non-owning package of data passed to audio processors
  template<int N>
  struct ProcessData {
    static constexpr int channels = N;

    std::array<AudioBufferHandle, channels> audio;
    midi::shared_vector<midi::AnyMidiEvent> midi;
    long nframes;

    ProcessData(std::array<AudioBufferHandle, channels> audio,
                midi::shared_vector<midi::AnyMidiEvent> midi,
                long nframes) noexcept;

    ProcessData(std::array<AudioBufferHandle, channels> audio,
                midi::shared_vector<midi::AnyMidiEvent> midi) noexcept;

    ProcessData(std::array<AudioBufferHandle, channels> audio) noexcept;

    ProcessData<0> midi_only();

    ProcessData audio_only();

    template<std::size_t NN>
    ProcessData<NN> redirect(const std::array<AudioBufferHandle, NN>& buf);

    ProcessData<1> redirect(const AudioBufferHandle& buf);

    /// Get only a slice of the audio.
    ///
    /// \param idx The index to start from
    /// \param length The number of frames to keep in the slice
    ///   If `length` is negative, `nframes - idx` will be used
    /// \requires parameter `idx` shall be in the range `[0, nframes)`, and
    /// `length` shall be in range `[0, nframes - idx]`
    ProcessData slice(int idx, int length = -1);

    std::array<float*, channels> raw_audio_buffers();
  };

  using TestType = std::vector<struct Tag>;

  /// Non-owning package of data passed to audio processors
  template<>
  struct ProcessData<0> {
    static constexpr int channels = 0;

    midi::shared_vector<midi::AnyMidiEvent> midi;
    long nframes;

    ProcessData(midi::shared_vector<midi::AnyMidiEvent> midi, long nframes) noexcept;

    template<std::size_t NN>
    ProcessData<NN> redirect(const std::array<AudioBufferHandle, NN>& buf);
    ProcessData<1> redirect(const AudioBufferHandle& buf);

    std::array<float*, 0> raw_audio_buffers();
  };

  /// Non-owning package of data passed to audio processors
  template<>
  struct ProcessData<1> {
    static constexpr int channels = 1;

    AudioBufferHandle audio;
    midi::shared_vector<midi::AnyMidiEvent> midi;
    long nframes;

    ProcessData(std::array<AudioBufferHandle, channels> audio,
                midi::shared_vector<midi::AnyMidiEvent> midi,
                long nframes) noexcept;

    ProcessData(std::array<AudioBufferHandle, channels> audio,
                midi::shared_vector<midi::AnyMidiEvent> midi) noexcept;

    ProcessData(std::array<AudioBufferHandle, channels> audio) noexcept;

    ProcessData(AudioBufferHandle audio,
                midi::shared_vector<midi::AnyMidiEvent> midi,
                long nframes) noexcept;

    ProcessData(AudioBufferHandle audio, midi::shared_vector<midi::AnyMidiEvent> midi) noexcept;

    ProcessData(AudioBufferHandle audio) noexcept;

    ProcessData<0> midi_only();
    ProcessData audio_only();

    template<std::size_t NN>
    ProcessData<NN> redirect(const std::array<AudioBufferHandle, NN>& buf);
    ProcessData<1> redirect(const AudioBufferHandle& buf);

    /// Get only a slice of the audio.
    ///
    /// \param idx The index to start from
    /// \param length The number of frames to keep in the slice
    ///   If `length` is negative, `nframes - idx` will be used
    /// \requires parameter `idx` shall be in the range `[0, nframes)`, and
    /// `length` shall be in range `[0, nframes - idx]`
    ProcessData slice(int idx, int length = -1);

    std::array<float*, channels> raw_audio_buffers();
  };


} // namespace otto::core::audio

// kak: other_file=processor.inl

#include "processor.inl"
