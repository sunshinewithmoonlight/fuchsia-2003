// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "use_h264_decoder.h"

#include "codec_client.h"
#include "util.h"

#include <fbl/auto_call.h>
#include <garnet/lib/media/raw_video_writer/raw_video_writer.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fxl/arraysize.h>
#include <lib/fxl/logging.h>

#include <stdint.h>
#include <string.h>
#include <thread>

namespace {

constexpr bool kRawVideoWriterEnabled = true;

// This example only has one stream_lifetime_ordinal which is 1.
//
// TODO(dustingreen): actually re-use the Codec instance for at least one more
// stream, even if it's just to decode the same data again.
constexpr uint64_t kStreamLifetimeOrdinal = 1;

constexpr uint8_t kLongStartCodeArray[] = {0x00, 0x00, 0x00, 0x01};
constexpr uint8_t kShortStartCodeArray[] = {0x00, 0x00, 0x01};

// If readable_bytes is 0, that's considered a "start code", to allow the caller
// to terminate a NAL the same way regardless of whether another start code is
// found or the end of the buffer is found.
//
// ptr has readable_bytes of data - the function only evaluates whether there is
// a start code at the begining of the data at ptr.
//
// readable_bytes - the caller indicates how many bytes are readable starting at
// ptr.
//
// *start_code_size_bytes will have length of the start code in bytes when the
// function returns true - unchanged otherwise.  Normally this would be 3 or 4,
// but a 0 is possible if readable_bytes is 0.
bool is_start_code(uint8_t* ptr, size_t readable_bytes,
                   size_t* start_code_size_bytes_out) {
  if (readable_bytes == 0) {
    *start_code_size_bytes_out = 0;
    return true;
  }
  if (readable_bytes >= 4) {
    if (!memcmp(ptr, kLongStartCodeArray, sizeof(kLongStartCodeArray))) {
      *start_code_size_bytes_out = 4;
      return true;
    }
  }
  if (readable_bytes >= 3) {
    if (!memcmp(ptr, kShortStartCodeArray, sizeof(kShortStartCodeArray))) {
      *start_code_size_bytes_out = 3;
      return true;
    }
  }
  return false;
}

// Test-only.  Not for production use.  Caller must ensure there are at least 5
// bytes at nal_unit.
uint8_t GetNalUnitType(const uint8_t* nal_unit) {
  // Also works with 4-byte startcodes.
  static const uint8_t start_code[3] = {0, 0, 1};
  uint8_t* next_start = static_cast<uint8_t*>(memmem(nal_unit, 5, start_code,
                                                     sizeof(start_code))) +
                        sizeof(start_code);
  return *next_start & 0xf;
}

static inline constexpr uint32_t make_fourcc(uint8_t a, uint8_t b, uint8_t c,
                                             uint8_t d) {
  return (static_cast<uint32_t>(d) << 24) | (static_cast<uint32_t>(c) << 16) |
         (static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
}

}  // namespace

void use_h264_decoder(fuchsia::mediacodec::CodecFactoryPtr codec_factory,
                      const std::string& input_file,
                      const std::string& output_file,
                      uint8_t md_out[SHA256_DIGEST_LENGTH],
                      std::vector<std::pair<bool, uint64_t>>* timestamps_out) {
  VLOGF("use_h264_decoder()\n");
  FXL_DCHECK(!timestamps_out || timestamps_out->empty());
  memset(md_out, 0, SHA256_DIGEST_LENGTH);
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  loop.StartThread("use_h264_decoder_loop");

  // payload data for bear.h264 is 00 00 00 01 start code before each NAL, with
  // SPS / PPS NALs and also frame NALs.  We deliver to Codec NAL-by-NAL without
  // the start code, since the Codec packet
  VLOGF("reading h264 file...\n");
  size_t input_size;
  std::unique_ptr<uint8_t[]> input_bytes =
      read_whole_file(input_file.c_str(), &input_size);
  VLOGF("done reading h264 file.\n");

  // TODO(dustingreen): Do this before binding the codec_factory.
  codec_factory.set_error_handler([] {
    // TODO(dustingreen): get and print CodecFactory channel epitaph once that's
    // possible.
    LOGF("codec_factory failed - unexpected\n");
  });

  // Since the .h264 file has SPS + PPS NALs in addition to frame NALs, we don't
  // use codec_oob_bytes for this stream.
  //
  // TODO(dustingreen): Determine for .mp4 or similar which don't have SPS / PPS
  // in band whether .mp4 provides ongoing OOB data, or just at the start, and
  // document in codec.fidl how that's to be handled.

  VLOGF("before CodecClient::CodecClient()...\n");
  CodecClient codec_client(&loop);
  VLOGF("before codec_factory->CreateDecoder().\n");
  // TODO(dustingreen): Do this from codec_factory's FIDL thread.
  codec_factory->CreateDecoder(
      fuchsia::mediacodec::CreateDecoder_Params{
          .input_details.format_details_version_ordinal = 0,
          .input_details.mime_type = "video/h264",
          // This is required for timestamp_ish values to transit the Codec.
          .promise_separate_access_units_on_input = true,
      },
      codec_client.GetTheRequestOnce());
  VLOGF("before codec_client.Start()...\n");
  codec_client.Start();

  // TODO(dustingreen): Do this from use_h264_decoder_loop thread.
  codec_factory.Unbind();

  VLOGF("before starting in_thread...\n");
  std::unique_ptr<std::thread> in_thread = std::make_unique<std::thread>(
      [&codec_client, &input_bytes, input_size]() {
        // We assign fake PTS values starting at 0 partly to verify that 0 is
        // treated as a valid PTS.
        uint64_t input_frame_pts_counter = 0;
        // Raw .h264 has start code 00 00 01 or 00 00 00 01 before each NAL, and
        // the start codes don't alias in the middle of NALs, so we just scan
        // for NALs and send them in to the decoder.
        auto queue_access_unit = [&codec_client, &input_bytes,
                                  &input_frame_pts_counter](uint8_t* bytes,
                                                            size_t byte_count) {
          size_t bytes_so_far = 0;
          // printf("queuing offset: %ld byte_count: %zu\n", bytes -
          // input_bytes.get(), byte_count);
          while (bytes_so_far != byte_count) {
            std::unique_ptr<fuchsia::mediacodec::CodecPacket> packet =
                codec_client.BlockingGetFreeInputPacket();
            const CodecBuffer& buffer =
                codec_client.GetInputBufferByIndex(packet->header.packet_index);
            size_t bytes_to_copy =
                std::min(byte_count - bytes_so_far, buffer.size_bytes());
            packet->stream_lifetime_ordinal = kStreamLifetimeOrdinal;
            packet->start_offset = 0;
            packet->valid_length_bytes = bytes_to_copy;

            packet->has_timestamp_ish = false;
            packet->timestamp_ish = 0;
            if (bytes_so_far == 0) {
              uint8_t nal_unit_type = GetNalUnitType(bytes);
              if (nal_unit_type == 1 || nal_unit_type == 5) {
                packet->has_timestamp_ish = true;
                packet->timestamp_ish = input_frame_pts_counter++;
              }
            }

            packet->start_access_unit = (bytes_so_far == 0);
            packet->known_end_access_unit =
                (bytes_so_far + bytes_to_copy == byte_count);
            memcpy(buffer.base(), bytes + bytes_so_far, bytes_to_copy);
            codec_client.QueueInputPacket(std::move(packet));
            bytes_so_far += bytes_to_copy;
          }
        };
        for (size_t i = 0; i < input_size;) {
          size_t start_code_size_bytes;
          if (!is_start_code(&input_bytes[i], input_size - i,
                             &start_code_size_bytes)) {
            if (i == 0) {
              Exit(
                  "Didn't find a start code at the start of the file, and this "
                  "example doesn't scan forward (for now).");
            } else {
              Exit(
                  "Fell out of sync somehow - previous NAL offset + previous "
                  "NAL length not a start code.");
            }
          }
          if (i + start_code_size_bytes == input_size) {
            Exit("Start code at end of file unexpected");
          }
          size_t nal_start_offset = i + start_code_size_bytes;
          // Scan for end of NAL.  The end of NAL can be because we're out of
          // data, or because we hit another start code.
          size_t find_end_iter = nal_start_offset;
          size_t ignore_start_code_size_bytes;
          while (find_end_iter <= input_size &&
                 !is_start_code(&input_bytes[find_end_iter],
                                input_size - find_end_iter,
                                &ignore_start_code_size_bytes)) {
            find_end_iter++;
          }
          FXL_DCHECK(find_end_iter <= input_size);
          if (find_end_iter == nal_start_offset) {
            Exit("Two adjacent start codes unexpected.");
          }
          FXL_DCHECK(find_end_iter > nal_start_offset);
          size_t nal_length = find_end_iter - nal_start_offset;
          queue_access_unit(&input_bytes[i],
                            start_code_size_bytes + nal_length);
          // start code + NAL payload
          i += start_code_size_bytes + nal_length;
        }

        // Send through QueueInputEndOfStream().
        codec_client.QueueInputEndOfStream(kStreamLifetimeOrdinal);
        // input thread done
      });

  // Separate thread to process the output.
  //
  // codec_client outlives the thread.
  std::unique_ptr<std::thread> out_thread = std::make_unique<
      std::thread>([&codec_client, output_file, md_out, &timestamps_out]() {
    // The codec_client lock_ is not held for long durations in here, which is
    // good since we're using this thread to do things like write to an output
    // file.
    media::RawVideoWriter<kRawVideoWriterEnabled> raw_video_writer(
        output_file.c_str());
    SHA256_CTX sha256_ctx;
    SHA256_Init(&sha256_ctx);
    // We allow the server to send multiple output format updates if it wants;
    // see implementation of BlockingGetEmittedOutput() which will hide
    // multiple configs before the first packet from this code.
    //
    // In this example, we only deal with one output format once we start seeing
    // stream output data show up, since our raw_video_writer is only really
    // meant to store one format per file.
    std::shared_ptr<const fuchsia::mediacodec::CodecOutputConfig> stream_config;
    const fuchsia::mediacodec::VideoUncompressedFormat* raw = nullptr;
    while (true) {
      std::unique_ptr<CodecOutput> output =
          codec_client.BlockingGetEmittedOutput();
      if (output->stream_lifetime_ordinal() != kStreamLifetimeOrdinal) {
        Exit(
            "server emitted a stream_lifetime_ordinal that client didn't set "
            "on any input");
      }
      if (output->end_of_stream()) {
        VLOGF("output end_of_stream() - done with output\n");
        // Just "break;" would be more fragile under code modification.
        goto end_of_output;
      }

      const fuchsia::mediacodec::CodecPacket& packet = output->packet();
      // "packet" will live long enough because ~cleanup runs before ~output.
      auto cleanup = fbl::MakeAutoCall([&codec_client, &packet] {
        // Using an auto call for this helps avoid losing track of the
        // output_buffer.
        //
        // If the omx_state_ or omx_state_desired_ isn't correct,
        // UseOutputBuffer() will fail.  The only way that can happen here is
        // if the OMX codec transitioned states unilaterally without any set
        // state command, so if that occurs, exit.
        codec_client.RecycleOutputPacket(packet.header);
      });
      std::shared_ptr<const fuchsia::mediacodec::CodecOutputConfig> config =
          output->config();
      // This will remain live long enough because this thread is the only
      // thread that re-allocates output buffers.
      const CodecBuffer& buffer =
          codec_client.GetOutputBufferByIndex(packet.header.packet_index);

      if (stream_config &&
          (config->format_details.format_details_version_ordinal !=
           stream_config->format_details.format_details_version_ordinal)) {
        Exit(
            "codec server unexpectedly changed output format mid-stream - "
            "unexpected for this stream");
      }

      if (packet.valid_length_bytes == 0) {
        // The server should not generate any empty packets.
        Exit("broken server sent empty packet");
      }

      // We have a non-empty packet of the stream.

      if (!stream_config) {
        // Every output has a config.  This happens exactly once.
        stream_config = config;
        const fuchsia::mediacodec::CodecFormatDetails& format =
            stream_config->format_details;
        if (!format.domain->is_video()) {
          Exit("!format.domain.is_video()");
        }
        const fuchsia::mediacodec::VideoFormat& video_format =
            format.domain->video();
        if (!video_format.is_uncompressed()) {
          Exit("!video.is_uncompressed()");
        }
        raw = &video_format.uncompressed();
        if (raw->fourcc != make_fourcc('N', 'V', '1', '2')) {
          Exit("fourcc != NV12");
        }
        size_t y_size =
            raw->primary_height_pixels * raw->primary_line_stride_bytes;
        if (raw->secondary_start_offset < y_size) {
          Exit("raw.secondary_start_offset < y_size");
        }
        // NV12 requires UV be same line stride as Y.
        size_t total_size =
            raw->secondary_start_offset +
            raw->primary_height_pixels / 2 * raw->primary_line_stride_bytes;
        if (packet.valid_length_bytes < total_size) {
          Exit("packet.valid_length_bytes < total_size");
        }
        SHA256_Update_VideoParameters(&sha256_ctx, *raw);
      }

      if (!output_file.empty()) {
        raw_video_writer.WriteNv12(
            raw->primary_width_pixels, raw->primary_height_pixels,
            raw->primary_line_stride_bytes,
            buffer.base() + packet.start_offset + raw->primary_start_offset,
            raw->secondary_start_offset - raw->primary_start_offset);
      }

      // PTS values are separately verified by use_h264_decoder_test since it'll
      // be nice to know separately if they're broken and how vs. frame format
      // and frame pixel data being broken, especially if there's just one
      // broken run that can't easily be reproduced.
      if (timestamps_out) {
        timestamps_out->emplace_back(
            std::make_pair(packet.has_timestamp_ish, packet.timestamp_ish));
      }

      // Y
      uint8_t* y_src =
          buffer.base() + packet.start_offset + raw->primary_start_offset;
      for (uint32_t y_iter = 0; y_iter < raw->primary_height_pixels; y_iter++) {
        SHA256_Update(&sha256_ctx, y_src, raw->primary_width_pixels);
        y_src += raw->primary_line_stride_bytes;
      }
      // UV
      uint8_t* uv_src =
          buffer.base() + packet.start_offset + raw->secondary_start_offset;
      for (uint32_t uv_iter = 0; uv_iter < raw->primary_height_pixels / 2;
           uv_iter++) {
        // NV12 requires eacy UV line be same width as a Y line, and same stride
        // as a Y line.
        SHA256_Update(&sha256_ctx, uv_src, raw->primary_width_pixels);
        uv_src += raw->primary_line_stride_bytes;
      }
    }
  end_of_output:;
    if (!SHA256_Final(md_out, &sha256_ctx)) {
      assert(false);
    }
    printf("output thread done\n");
    // output thread done
    // ~raw_video_writer
  });

  // decode for a bit...  in_thread, loop, out_thread, and the codec itself are
  // taking care of it.

  // First wait for the input thread to be done feeding input data.  Before the
  // in_thread terminates, it'll have sent in a last empty EOS input buffer.
  VLOGF("before in_thread->join()...\n");
  in_thread->join();
  VLOGF("after in_thread->join()\n");

  // The EOS queued as an input buffer should cause the codec to output an EOS
  // output buffer, at which point out_thread should terminate, after it has
  // finalized the output file.
  VLOGF("before out_thread->join()...\n");
  out_thread->join();
  VLOGF("after out_thread->join()\n");

  // Because CodecClient posted work to the loop which captured the CodecClient
  // as "this", it's important that we ensure that all such work is done trying
  // to run before we delete CodecClient.  We need to know that the work posted
  // using PostSerial() won't be trying to touch the channel or pointers that
  // are owned by CodecClient before we close the channel or destruct
  // CodecClient (which happens before ~loop).
  //
  // We call loop.Quit();loop.JoinThreads(); before codec_client.Stop() because
  // there can be at least a RecycleOutputPacket() still working its way toward
  // the Codec (via the loop) at this point, so doing
  // loop.Quit();loop.JoinThreads(); first avoids potential FIDL message send
  // errors.  We're done decoding so we don't care whether any remaining queued
  // messages toward the codec actually reach the codec.
  //
  // We use loop.Quit();loop.JoinThreads(); instead of loop.Shutdown() because
  // we don't want the Shutdown() side-effect of failing the channel bindings.
  // The Shutdown() will happen later.
  //
  // By ensuring that the loop is done running code before closing the channel
  // (or loop.Shutdown()), we can close the channel cleanly and avoid mitigation
  // of expected normal channel closure (or loop.Shutdown()) in any code that
  // runs on the loop.  This way, unexpected channel failure is the only case to
  // worry about.
  VLOGF("before loop.Quit()\n");
  loop.Quit();
  VLOGF("before loop.JoinThreads()...\n");
  loop.JoinThreads();
  VLOGF("after loop.JoinThreads()\n");

  // Close the channel explicitly (just so we can more easily print messages
  // before and after vs. ~codec_client).
  VLOGF("before codec_client stop...\n");
  codec_client.Stop();
  VLOGF("after codec_client stop.\n");

  // loop.Shutdown() the rest of the way explicitly (just so we can more easily
  // print messages before and after vs. ~loop).  If we did this before
  // codec_client.Stop() it would cause the channel bindings to fail because
  // async waits are failed as cancelled during Shutdown().
  VLOGF("before loop.Shutdown()...\n");
  loop.Shutdown();
  VLOGF("after loop.Shutdown()\n");

  // The FIDL loop isn't running any more and the channels are closed.  There
  // are no other threads left that were started by this function.  We can just
  // delete stuff now.

  // success
  // ~codec_client
  // ~loop
  // ~codec_factory
  return;
}
