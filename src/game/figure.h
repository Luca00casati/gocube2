#pragma once
// A jointed figure made of boxes (ninjas; players' figures later), animated
// from how it moves: the Godot version's Figure. Feet at the origin, facing
// -z, 1.8 m tall.
//
// Joints: hips -> torso (bends at the waist) -> head, shoulders -> upper arm
// -> elbow -> forearm, and hips -> thigh -> knee -> shin. Each frame the owner
// sets the inputs; the figure blends towards the pose they ask for (joint by
// joint), so changes never snap. swing() plays a strike with the right arm.

namespace gc {

struct Figure {
	static constexpr float HIP_Y = 0.76f, THIGH = 0.38f, SHIN = 0.38f, TORSO = 0.62f;
	static constexpr float UPPER_ARM = 0.3f, FOREARM = 0.32f;
	static constexpr float STRIDE = 1.5f;    // metres a full walk cycle (two steps)
	static constexpr float RUN_SPEED = 5.0f; // the stride is fully stretched at this speed
	static constexpr float SWING_TIME = 0.4f;

	enum Joint { HIPS, TORSO_J, HEAD, ARM_L, ARM_R, ELBOW_L, ELBOW_R, LEG_L, LEG_R, KNEE_L, KNEE_R, JOINTS };

	// Inputs.
	float move_speed = 0;  // horizontal m/s
	bool on_floor = true, sneaking = false, lying = false;
	float pitch = 0;       // looking up (+) or down (-)
	bool ninja = false;    // runs leaning forward with the arms swept back

	// The pose (Euler angles, Godot's YXZ order) and the hips' height.
	float rot[JOINTS][3] = {};
	float hips_y = HIP_Y;
	float lie = 0;         // 0 standing .. 1 lying down

	void swing(); // one asked for during another plays right after it
	bool swinging() const { return swing_ >= 0; }
	void update(float dt);

private:
	void pose(float dt);
	float phase_ = 0, amount_ = 0, air_ = 0, sneak_ = 0;
	float swing_ = -1;
	bool swing_next_ = false;
};

} // namespace gc
