/*
 * Copyright (C) 2003-2015 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "RecorderOutputPlugin.hxx"
#include "../OutputAPI.hxx"
#include "../Wrapper.hxx"
#include "encoder/EncoderPlugin.hxx"
#include "encoder/EncoderList.hxx"
#include "config/ConfigError.hxx"
#include "Log.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileSystem.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "system/fd_util.h"
#include "open.h"

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

struct RecorderOutput {
	AudioOutput base;

	/**
	 * The configured encoder plugin.
	 */
	Encoder *encoder;

	/**
	 * The destination file name.
	 */
	AllocatedPath path;

	/**
	 * The destination file descriptor.
	 */
	int fd;

	/**
	 * The buffer for encoder_read().
	 */
	char buffer[32768];

	RecorderOutput()
		:base(recorder_output_plugin),
		 path(AllocatedPath::Null()) {}

	bool Initialize(const config_param &param, Error &error_r) {
		return base.Configure(param, error_r);
	}

	static RecorderOutput *Create(const config_param &param, Error &error);

	bool Configure(const config_param &param, Error &error);

	bool Open(AudioFormat &audio_format, Error &error);
	void Close();

	bool WriteToFile(const void *data, size_t length, Error &error);

	/**
	 * Writes pending data from the encoder to the output file.
	 */
	bool EncoderToFile(Error &error);

	void SendTag(const Tag &tag);

	size_t Play(const void *chunk, size_t size, Error &error);
};

static constexpr Domain recorder_output_domain("recorder_output");

inline bool
RecorderOutput::Configure(const config_param &param, Error &error)
{
	/* read configuration */

	const char *encoder_name =
		param.GetBlockValue("encoder", "vorbis");
	const auto encoder_plugin = encoder_plugin_get(encoder_name);
	if (encoder_plugin == nullptr) {
		error.Format(config_domain,
			     "No such encoder: %s", encoder_name);
		return false;
	}

	path = param.GetBlockPath("path", error);
	if (path.IsNull()) {
		if (!error.IsDefined())
			error.Set(config_domain, "'path' not configured");
		return false;
	}

	/* initialize encoder */

	encoder = encoder_init(*encoder_plugin, param, error);
	if (encoder == nullptr)
		return false;

	return true;
}

RecorderOutput *
RecorderOutput::Create(const config_param &param, Error &error)
{
	RecorderOutput *recorder = new RecorderOutput();

	if (!recorder->Initialize(param, error)) {
		delete recorder;
		return nullptr;
	}

	if (!recorder->Configure(param, error)) {
		delete recorder;
		return nullptr;
	}

	return recorder;
}

static void
recorder_output_finish(AudioOutput *ao)
{
	RecorderOutput *recorder = (RecorderOutput *)ao;

	encoder_finish(recorder->encoder);
	delete recorder;
}

inline bool
RecorderOutput::WriteToFile(const void *_data, size_t length, Error &error)
{
	assert(length > 0);

	const uint8_t *data = (const uint8_t *)_data, *end = data + length;

	while (true) {
		ssize_t nbytes = write(fd, data, end - data);
		if (nbytes > 0) {
			data += nbytes;
			if (data == end)
				return true;
		} else if (nbytes == 0) {
			/* shouldn't happen for files */
			error.Set(recorder_output_domain,
				  "write() returned 0");
			return false;
		} else if (errno != EINTR) {
			error.FormatErrno("Failed to write to '%s'",
					  path.c_str());
			return false;
		}
	}
}

inline bool
RecorderOutput::EncoderToFile(Error &error)
{
	assert(fd >= 0);

	while (true) {
		/* read from the encoder */

		size_t size = encoder_read(encoder, buffer, sizeof(buffer));
		if (size == 0)
			return true;

		/* write everything into the file */

		if (!WriteToFile(buffer, size, error))
			return false;
	}
}

inline bool
RecorderOutput::Open(AudioFormat &audio_format, Error &error)
{
	/* create the output file */

	fd = OpenFile(path,
		      O_CREAT|O_WRONLY|O_TRUNC|O_BINARY,
		      0666);
	if (fd < 0) {
		error.FormatErrno("Failed to create '%s'", path.c_str());
		return false;
	}

	/* open the encoder */

	if (!encoder_open(encoder, audio_format, error)) {
		close(fd);
		RemoveFile(path);
		return false;
	}

	if (!EncoderToFile(error)) {
		encoder_close(encoder);
		close(fd);
		RemoveFile(path);
		return false;
	}

	return true;
}

inline void
RecorderOutput::Close()
{
	/* flush the encoder and write the rest to the file */

	if (encoder_end(encoder, IgnoreError()))
		EncoderToFile(IgnoreError());

	/* now really close everything */

	encoder_close(encoder);

	close(fd);
}

inline void
RecorderOutput::SendTag(const Tag &tag)
{
	Error error;
	if (!encoder_pre_tag(encoder, error) ||
	    !EncoderToFile(error) ||
	    !encoder_tag(encoder, tag, error))
		LogError(error);
}

static void
recorder_output_send_tag(AudioOutput *ao, const Tag &tag)
{
	RecorderOutput &recorder = *(RecorderOutput *)ao;

	recorder.SendTag(tag);
}

inline size_t
RecorderOutput::Play(const void *chunk, size_t size, Error &error)
{
	return encoder_write(encoder, chunk, size, error) &&
		EncoderToFile(error)
		? size : 0;
}

typedef AudioOutputWrapper<RecorderOutput> Wrapper;

const struct AudioOutputPlugin recorder_output_plugin = {
	"recorder",
	nullptr,
	&Wrapper::Init,
	recorder_output_finish,
	nullptr,
	nullptr,
	&Wrapper::Open,
	&Wrapper::Close,
	nullptr,
	recorder_output_send_tag,
	&Wrapper::Play,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};
