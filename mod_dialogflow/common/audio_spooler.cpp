#include "audio_spooler.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <inttypes.h>
#include <system_error>

#if defined(__has_include)
#  if __has_include(<filesystem>)
#    include <filesystem>
namespace fs = std::filesystem;
#  elif __has_include(<experimental/filesystem>)
#    include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#  else
#    error "No filesystem implementation available"
#  endif
#else
#  include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

static std::string sanitize_tag(const std::string& tag) {
	std::string out;
	out.reserve(tag.size());
	for (char c : tag) {
		unsigned char uc = static_cast<unsigned char>(c);
		if (std::isalnum(uc) || c == '-' || c == '_') {
			out.push_back(c);
		} else if (std::isspace(uc)) {
			out.push_back('_');
		}
	}
	if (out.empty()) {
		out.assign("snippet");
	}
	return out;
}

static std::string timestamp_now() {
	switch_time_t now = switch_micro_time_now();
	time_t seconds = static_cast<time_t>(now / 1000000);
	struct tm tm_now;
#ifdef _WIN32
	gmtime_s(&tm_now, &seconds);
#else
	gmtime_r(&seconds, &tm_now);
#endif
	char buf[64];
	unsigned int micros = static_cast<unsigned int>(now % 1000000);
	switch_snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02d_%06u",
		tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
		tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, micros);
	return std::string(buf);
}

static double samples_to_ms(uint64_t samples, uint32_t sampleRate) {
	if (!sampleRate) {
		return 0.0;
	}
	double seconds = static_cast<double>(samples) / static_cast<double>(sampleRate);
	return seconds * 1000.0;
}

switch_status_t AudioSpooler::configure(switch_core_session_t* session, const std::string& sid, uint32_t rate) {
	switch_channel_t* channel = switch_core_session_get_channel(session);
	const char* secVar = switch_channel_get_variable(channel, "DIALOGFLOW_SPOOL_SEC");
	int horizonSec = secVar ? atoi(secVar) : 0;
	if (horizonSec <= 0) {
		enabled = SWITCH_FALSE;
		return SWITCH_STATUS_SUCCESS;
	}

	sessionId = sid;
	sampleRate = rate;
	horizonMs = static_cast<uint32_t>(horizonSec * 1000);
	if (horizonMs == 0) horizonMs = 1000;

	const char* chunkVar = switch_channel_get_variable(channel, "DIALOGFLOW_SPOOL_CHUNK_MS");
	int chunkMsCandidate = chunkVar ? atoi(chunkVar) : 2000;
	if (chunkMsCandidate < 200) chunkMsCandidate = 200;
	if (chunkMsCandidate > static_cast<int>(horizonMs)) chunkMsCandidate = static_cast<int>(horizonMs);
	chunkMs = static_cast<uint32_t>(chunkMsCandidate);

	const char* dirVar = switch_channel_get_variable(channel, "DIALOGFLOW_SPOOL_DIR");
	rootDir = (dirVar && *dirVar) ? dirVar : "/tmp/dialogflow-spool";

	preserve = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_SPOOL_PRESERVE"));

	samplesPerChunk = std::max<uint64_t>(1, (static_cast<uint64_t>(sampleRate) * chunkMs) / 1000);
	maxSamples = std::max<uint64_t>(samplesPerChunk, (static_cast<uint64_t>(sampleRate) * horizonMs) / 1000);

	sessionDir = rootDir + "/" + sid;
	chunkDir = sessionDir + "/chunks";
	snippetDir = sessionDir + "/snippets";

	std::error_code ec;
	fs::remove_all(sessionDir, ec);
	ec.clear();
	fs::create_directories(chunkDir, ec);
	if (ec) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"AudioSpooler: failed to create chunk directory '%s': %s\n",
			chunkDir.c_str(), ec.message().c_str());
		enabled = SWITCH_FALSE;
		return SWITCH_STATUS_FALSE;
	}
	ec.clear();
	fs::create_directories(snippetDir, ec);
	if (ec) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"AudioSpooler: failed to create snippet directory '%s': %s\n",
			snippetDir.c_str(), ec.message().c_str());
		enabled = SWITCH_FALSE;
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_variable(channel, "DF_SPOOL_DIR", sessionDir.c_str());
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
		"Audio spool enabled (horizon=%ums chunk=%ums dir=%s preserve=%s)\n",
		horizonMs, chunkMs, sessionDir.c_str(), preserve ? "true" : "false");

	enabled = SWITCH_TRUE;
	totalSamples = 0;
	chunkCounter = 0;
	currentChunkSamples = 0;
	currentChunkStartSample = 0;
	chunks.clear();
	currentStream.reset();
	return SWITCH_STATUS_SUCCESS;
}

void AudioSpooler::cleanup() {
	if (currentStream && currentStream->is_open()) {
		currentStream->close();
	}
	currentStream.reset();
	if (!preserve && !sessionDir.empty()) {
		std::error_code ec;
		fs::remove_all(sessionDir, ec);
	}
	chunks.clear();
	enabled = SWITCH_FALSE;
}

uint64_t AudioSpooler::earliestSample() const {
	if (chunks.empty()) return totalSamples;
	return chunks.front().startSample;
}

uint64_t AudioSpooler::latestSample() const {
	return totalSamples;
}

void AudioSpooler::flushActive() {
	if (currentStream) {
		currentStream->flush();
	}
}

switch_status_t AudioSpooler::openNewChunk(switch_core_session_t* session) {
	if (currentStream && currentStream->is_open()) {
		currentStream->close();
	}

	currentChunkStartSample = totalSamples;
	currentChunkSamples = 0;

	char filename[64];
	switch_snprintf(filename, sizeof(filename), "chunk-%05" PRIu64 ".raw", chunkCounter++);
	std::string path = chunkDir + "/" + filename;

	currentStream = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
	if (!currentStream->is_open()) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"AudioSpooler: unable to open chunk file '%s'\n", path.c_str());
		currentStream.reset();
		return SWITCH_STATUS_FALSE;
	}

	ChunkInfo info;
	info.startSample = currentChunkStartSample;
	info.sampleCount = 0;
	info.path = path;
	info.finalised = false;
	chunks.push_back(info);
	return SWITCH_STATUS_SUCCESS;
}

void AudioSpooler::finalizeChunk() {
	if (currentStream && currentStream->is_open()) {
		currentStream->flush();
		currentStream->close();
	}
	if (!chunks.empty()) {
		chunks.back().finalised = true;
	}
	currentStream.reset();
	currentChunkSamples = 0;
}

void AudioSpooler::pruneOldChunks() {
	if (maxSamples == 0) return;
	uint64_t allowed = (totalSamples > maxSamples) ? (totalSamples - maxSamples) : 0;
	while (!chunks.empty()) {
		const ChunkInfo& front = chunks.front();
		uint64_t chunkEnd = front.startSample + front.sampleCount;
		if (chunkEnd > allowed) {
			break;
		}
		std::error_code ec;
		fs::remove(front.path, ec);
		chunks.pop_front();
	}
}

void AudioSpooler::ingest(switch_core_session_t* session, const int16_t* samples, size_t sampleCount) {
	if (enabled != SWITCH_TRUE || !sampleCount || !sampleRate) {
		totalSamples += sampleCount;
		return;
	}

	size_t offset = 0;
	while (offset < sampleCount) {
		if (!currentStream) {
			if (openNewChunk(session) != SWITCH_STATUS_SUCCESS) {
				return;
			}
		}

		size_t remainingInChunk = samplesPerChunk > currentChunkSamples
			? static_cast<size_t>(samplesPerChunk - currentChunkSamples)
			: 0;
		if (!remainingInChunk) {
			finalizeChunk();
			continue;
		}

		size_t toWrite = std::min(remainingInChunk, sampleCount - offset);
		currentStream->write(reinterpret_cast<const char*>(samples + offset),
			static_cast<std::streamsize>(toWrite * sizeof(int16_t)));
		if (!(*currentStream)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
				"AudioSpooler: failed writing chunk file '%s'\n", chunks.back().path.c_str());
			currentStream->clear();
			return;
		}

		currentChunkSamples += toWrite;
		chunks.back().sampleCount += toWrite;
		offset += toWrite;
		totalSamples += toWrite;

		if (currentChunkSamples >= samplesPerChunk) {
			finalizeChunk();
		}
	}

	pruneOldChunks();
}

switch_status_t AudioSpooler::readSamplesFromChunk(const ChunkInfo& chunk, uint64_t offsetSamples,
	uint64_t samplesToCopy, std::vector<int16_t>& out) {
	if (!samplesToCopy) return SWITCH_STATUS_SUCCESS;
	std::ifstream input(chunk.path, std::ios::binary);
	if (!input.is_open()) {
		return SWITCH_STATUS_FALSE;
	}
	input.seekg(static_cast<std::streamoff>(offsetSamples * sizeof(int16_t)), std::ios::beg);
	if (!input.good()) {
		return SWITCH_STATUS_FALSE;
	}
	std::vector<int16_t> tmp(samplesToCopy);
	input.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(samplesToCopy * sizeof(int16_t)));
	std::streamsize got = input.gcount();
	if (got <= 0) {
		return SWITCH_STATUS_FALSE;
	}
	size_t samplesRead = static_cast<size_t>(got / sizeof(int16_t));
	tmp.resize(samplesRead);
	out.insert(out.end(), tmp.begin(), tmp.end());
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t AudioSpooler::writeWav(const std::string& path, const std::vector<int16_t>& data) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return SWITCH_STATUS_FALSE;
	}

	uint16_t audioFormat = 1;
	uint16_t numChannels = 1;
	uint16_t bitsPerSample = 16;
	uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
	uint16_t blockAlign = numChannels * (bitsPerSample / 8);
	uint32_t dataSize = static_cast<uint32_t>(data.size() * sizeof(int16_t));
	uint32_t chunkSize = 36 + dataSize;

	output.write("RIFF", 4);
	output.write(reinterpret_cast<const char*>(&chunkSize), sizeof(chunkSize));
	output.write("WAVE", 4);
	output.write("fmt ", 4);
	uint32_t subchunk1Size = 16;
	output.write(reinterpret_cast<const char*>(&subchunk1Size), sizeof(subchunk1Size));
	output.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
	output.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
	output.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
	output.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
	output.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
	output.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
	output.write("data", 4);
	output.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
	if (dataSize) {
		output.write(reinterpret_cast<const char*>(data.data()), dataSize);
	}
	return output.good() ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_status_t AudioSpooler::capture(switch_core_session_t* session, uint64_t startMs, uint64_t durationMs,
	const std::string& tag, std::string& outPath, uint64_t& actualStartMs, uint64_t& actualEndMs) {
	if (enabled != SWITCH_TRUE || chunks.empty() || !sampleRate) {
		return SWITCH_STATUS_FALSE;
	}

	flushActive();

	if (!durationMs) durationMs = chunkMs;

	uint64_t requestedStartSamples = (startMs * sampleRate) / 1000;
	uint64_t requestedSamples = std::max<uint64_t>(1, (durationMs * sampleRate) / 1000);
	uint64_t requestedEndSamples = requestedStartSamples + requestedSamples;

	uint64_t earliest = earliestSample();
	uint64_t latest = latestSample();
	if (requestedStartSamples < earliest) {
		requestedStartSamples = earliest;
	}
	if (requestedEndSamples > latest) {
		requestedEndSamples = latest;
	}
	if (requestedStartSamples >= requestedEndSamples) {
		return SWITCH_STATUS_FALSE;
	}

	std::vector<int16_t> collected;
	collected.reserve(static_cast<size_t>(requestedEndSamples - requestedStartSamples));

	for (const auto& chunk : chunks) {
		uint64_t chunkStart = chunk.startSample;
		uint64_t chunkEnd = chunk.startSample + chunk.sampleCount;
		if (chunkEnd <= requestedStartSamples) {
			continue;
		}
		if (chunkStart >= requestedEndSamples) {
			break;
		}
		uint64_t from = std::max(chunkStart, requestedStartSamples);
		uint64_t to = std::min(chunkEnd, requestedEndSamples);
		uint64_t localOffset = from - chunkStart;
		uint64_t toCopy = to - from;
		if (readSamplesFromChunk(chunk, localOffset, toCopy, collected) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
				"AudioSpooler: failed reading chunk '%s'\n", chunk.path.c_str());
			return SWITCH_STATUS_FALSE;
		}
	}

	if (collected.empty()) {
		return SWITCH_STATUS_FALSE;
	}

	std::string safeTag = sanitize_tag(tag);
	std::string ts = timestamp_now();
	std::string filename = safeTag + "-" + ts + ".wav";
	outPath = snippetDir + "/" + filename;

	if (writeWav(outPath, collected) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"AudioSpooler: failed writing snippet '%s'\n", outPath.c_str());
		return SWITCH_STATUS_FALSE;
	}

	actualStartMs = static_cast<uint64_t>(samples_to_ms(requestedStartSamples, sampleRate));
	actualEndMs = static_cast<uint64_t>(samples_to_ms(requestedStartSamples + collected.size(), sampleRate));

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
		"AudioSpooler: captured snippet %s (start=%" PRIu64 "ms end=%" PRIu64 "ms duration=%" PRIu64 "ms)\n",
		outPath.c_str(), actualStartMs, actualEndMs, actualEndMs > actualStartMs ? actualEndMs - actualStartMs : 0);

	return SWITCH_STATUS_SUCCESS;
}
