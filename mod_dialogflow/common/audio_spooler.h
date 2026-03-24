#ifndef __AUDIO_SPOOLER_H__
#define __AUDIO_SPOOLER_H__

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class AudioSpooler {
public:
	struct ChunkInfo {
		uint64_t startSample = 0;
		uint64_t sampleCount = 0;
		std::string path;
		bool finalised = false;
	};

	switch_bool_t enabled = SWITCH_FALSE;
	std::string rootDir;
	std::string sessionDir;
	std::string chunkDir;
	std::string snippetDir;
	std::string sessionId;
	uint32_t sampleRate = 0;
	uint32_t chunkMs = 0;
	uint32_t horizonMs = 0;
	uint64_t samplesPerChunk = 0;
	uint64_t maxSamples = 0;
	uint64_t totalSamples = 0;
	uint64_t chunkCounter = 0;
	uint64_t currentChunkSamples = 0;
	uint64_t currentChunkStartSample = 0;
	bool preserve = false;
	std::unique_ptr<std::ofstream> currentStream;
	std::deque<ChunkInfo> chunks;

	switch_status_t configure(switch_core_session_t* session, const std::string& sid, uint32_t rate);
	void cleanup();
	uint64_t earliestSample() const;
	uint64_t latestSample() const;
	void flushActive();
	void ingest(switch_core_session_t* session, const int16_t* samples, size_t sampleCount);
	switch_status_t capture(switch_core_session_t* session, uint64_t startMs, uint64_t durationMs,
		const std::string& tag, std::string& outPath, uint64_t& actualStartMs, uint64_t& actualEndMs);

private:
	switch_status_t openNewChunk(switch_core_session_t* session);
	void finalizeChunk();
	void pruneOldChunks();
	switch_status_t readSamplesFromChunk(const ChunkInfo& chunk, uint64_t offsetSamples,
		uint64_t samplesToCopy, std::vector<int16_t>& out);
	switch_status_t writeWav(const std::string& path, const std::vector<int16_t>& data);
};

#endif
