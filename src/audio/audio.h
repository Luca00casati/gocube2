#pragma once
// The game's sounds, through XAudio2: a sound
// set's sound (content "sound_sets": a list of files per kind), a random one
// of its files, its pitch varied a little so repeats don't sound the same,
// heard from where it is (full volume within UNIT_SIZE blocks, then falling
// off, silent beyond MAX_DISTANCE). Two ambience loops (day and night)
// crossfade with the daylight; underwater, everything is muffled.
//
// Sounds (.wav) are read once and kept; at most VOICES play at a time (the
// oldest stops for a new one). How loud each is in each ear and the underwater
// filter are audio/mixing.h's. Without a sound device the game runs silent; when
// the device goes or the default one changes, sound starts again on the new one.

#include "content/content.h"

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace gc {

class Audio {
public:
	static constexpr float MAX_DISTANCE = 32.0f;
	static constexpr float UNIT_SIZE = 6.0f;
	static constexpr float AMBIENCE_DB = -14.0f;
	static constexpr int VOICES = 24;

	Audio();
	~Audio();
	Audio(const Audio &) = delete;
	Audio &operator=(const Audio &) = delete;

	// Opens the default sound device; false (and silent) if there is none.
	// `content` must outlive this (sound sets and mod folders).
	bool init(const Content &content, float master_volume = 1.0f);
	int voices() const; // playing now
	bool active() const;

	void play(const std::string &set, const std::string &kind, const float at[3], float volume_db = 0);
	// Each frame: where the ears are (the camera) and which way they face.
	void set_listener(const float position[3], const float forward[3]);
	// day: 0 at night .. 1 in full day.
	void update_ambience(float day);
	void set_underwater(bool underwater);
	// Volumes, 0..1: everything, the game's sounds, the ambience.
	void set_volumes(float master, float effects, float ambience);
	// Frees sounds that have finished (call once a frame).
	void update();

private:
	struct Impl;
	std::filesystem::path pick(const std::string &set, const std::string &kind); // a random file of it
	std::unique_ptr<Impl> impl_;
	const Content *content_ = nullptr;
	std::mt19937 random_{std::random_device{}()};
};

} // namespace gc
