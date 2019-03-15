/*
 * Author - Dustin Franklin (Nvidia Jetson Developer)
 * Modified by - Sahil Juneja, Kyle Stewart-Frantz
 *
 */

#include "ArmPlugin.h"
#include "PropPlugin.h"

#include "cudaMappedMemory.h"
#include "cudaPlanar.h"

#define PI 3.141592653589793238462643383279502884197169f

#define JOINT_MIN	-0.75f
#define JOINT_MAX	 2.0f

// Turn on velocity based control
#define VELOCITY_CONTROL false
#define VELOCITY_MIN -0.2f
#define VELOCITY_MAX  0.2f

// Define DQN API Settings

#define INPUT_CHANNELS 3
#define ALLOW_RANDOM true
#define DEBUG_DQN false
#define GAMMA 0.9f
#define EPS_START 0.9f // epsilon_start of random actions
#define EPS_END 0.01f // epsilon_end of random actions, initial value: 0.5f, reduce to keep it from continuing to explore too much once a solution is found
#define EPS_DECAY 15 // exponential decay of random actions, initial value: 200
#define NUM_ACTIONS 6
/*
/ TODO - Tune the following hyperparameters
/
*/

#define INPUT_WIDTH   128 // initial value: 512
#define INPUT_HEIGHT  128 // initial value: 512
#define OPTIMIZER "RMSprop"	// RMSprop, Adam, AdaGrad, None
#define LEARNING_RATE 0.2f // initial value: 0.0f
#define REPLAY_MEMORY 10000
#define BATCH_SIZE 64 // initial value: 8
#define USE_LSTM true // initial value: false
#define LSTM_SIZE 256 // initial value: 32

/*
/ TODO - Define Reward Parameters
/
*/

#define REWARD_WIN  2.0f // value for positive rewards
#define REWARD_LOSS -1.0f // value for negative rewards
#define ALPHA 0.25f // to compute the smoothed moving average distance to goal

// Define Object Names
#define WORLD_NAME "arm_world"
#define PROP_NAME  "tube"
#define GRIP_NAME  "gripper_middle"

// Define Collision Parameters
#define COLLISION_FILTER "ground_plane::link::collision"
#define COLLISION_ITEM   "tube::tube_link::tube_collision"
#define COLLISION_POINT  "arm::gripperbase::gripper_link"

// Animation Steps
#define ANIMATION_STEPS 1000

// Set Debug Mode
#define DEBUG false // set to false to view current accuracy
// Set project GOAL
#define GOAL 2
// GOAL 1: Any part of the robot arm touches the can (at least 90% accuracy)
// GOAL 2: The gripper base touches the can (at least 80% accuracy)

// Lock base rotation DOF (Add dof in header file if off)
#define LOCKBASE true

// float maxDistGoal = 0.0f;  // max distance to goal

namespace gazebo
{

// register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(ArmPlugin)


// constructor
ArmPlugin::ArmPlugin() : ModelPlugin(), cameraNode(new gazebo::transport::Node()), collisionNode(new gazebo::transport::Node())
{
	printf("ArmPlugin::ArmPlugin()\n");

	for( uint32_t n=0; n < DOF; n++ )
		resetPos[n] = 0.0f;

	resetPos[1] = 0.25;

	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] = resetPos[n]; //JOINT_MIN;
		vel[n] = 0.0f;
	}

	agent 	       = NULL;
	inputState       = NULL;
	inputBuffer[0]   = NULL;
	inputBuffer[1]   = NULL;
	inputBufferSize  = 0;
	inputRawWidth    = 0;
	inputRawHeight   = 0;
	actionJointDelta = 0.15f;
	actionVelDelta   = 0.1f;
	maxEpisodeLength = 100;
	episodeFrames    = 0;

	newState         = false;
	newReward        = false;
	endEpisode       = false;
	rewardHistory    = 0.0f;
	testAnimation    = true;
	loopAnimation    = false;
	animationStep    = 0;
	lastGoalDistance = 0.0f;
	avgGoalDelta     = 0.0f; // max distance to goal
	successfulGrabs = 0;
	totalRuns       = 0;
}


// Load
void ArmPlugin::Load(physics::ModelPtr _parent, sdf::ElementPtr /*_sdf*/)
{
	printf("ArmPlugin::Load('%s')\n", _parent->GetName().c_str());

	// Store the pointer to the model
	this->model = _parent;
	this->j2_controller = new physics::JointController(model);

	// Create our node for camera communication
	cameraNode->Init();

	/*
	/ TODO - Subscribe to camera topic
	*/
	cameraSub = cameraNode->Subscribe("/gazebo/arm_world/camera/link/camera/image", &ArmPlugin::onCameraMsg, this);// topic_name, callback_function, class_instance



	//cameraSub = None;

	// Create our node for collision detection
	collisionNode->Init();

	/*
	/ TODO - Subscribe to prop collision topic
	*/
	collisionSub = collisionNode->Subscribe("/gazebo/arm_world/tube/tube_link/my_contact", &ArmPlugin::onCollisionMsg, this);

	//collisionSub = None;

	// Listen to the update event. This event is broadcast every simulation iteration.
	this->updateConnection = event::Events::ConnectWorldUpdateBegin(boost::bind(&ArmPlugin::OnUpdate, this, _1));
}


// CreateAgent
bool ArmPlugin::createAgent()
{
	if( agent != NULL )
		return true;


	/*
	/ TODO - Create DQN Agent
	*/
    // Create reinforcement learner agent in pyTorch using API
    agent = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, NUM_ACTIONS, OPTIMIZER, LEARNING_RATE, REPLAY_MEMORY, BATCH_SIZE, GAMMA, EPS_START, EPS_END, EPS_DECAY, USE_LSTM, LSTM_SIZE, ALLOW_RANDOM, DEBUG_DQN);


	//agent = NULL;

	if( !agent )
	{
		printf("ArmPlugin - failed to create DQN agent\n");
		return false;
	}

	// Allocate the python tensor for passing the camera state

	inputState = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);

	if( !inputState )
	{
		printf("ArmPlugin - failed to allocate %ux%ux%u Tensor\n", INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
		return false;
	}

	return true;
}



// onCameraMsg
void ArmPlugin::onCameraMsg(ConstImageStampedPtr &_msg)
{
	// don't process the image if the agent hasn't been created yet
	if( !agent )
		return;

	// check the validity of the message contents
	if( !_msg )
	{
		printf("ArmPlugin - recieved NULL message\n");
		return;
	}

	// retrieve image dimensions

	const int width  = _msg->image().width();
	const int height = _msg->image().height();
	const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
	const int size   = _msg->image().data().size();

	if( bpp != 24 )
	{
		printf("ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
		return;
	}

	// allocate temp image if necessary
	if( !inputBuffer[0] || size != inputBufferSize )
	{
		if( !cudaAllocMapped(&inputBuffer[0], &inputBuffer[1], size) )
		{
			printf("ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
			return;
		}

		printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);

		inputBufferSize = size;
		inputRawWidth   = width;
		inputRawHeight  = height;
	}

	memcpy(inputBuffer[0], _msg->image().data().c_str(), inputBufferSize);
	newState = true;

	if(DEBUG){printf("camera %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);}

}


// onCollisionMsg
void ArmPlugin::onCollisionMsg(ConstContactsPtr &contacts)
{
	//if(DEBUG){printf("collision callback (%u contacts)\n", contacts->contact_size());}

	if( testAnimation )
		return;

	for (unsigned int i = 0; i < contacts->contact_size(); ++i)
	{
		if( strcmp(contacts->contact(i).collision2().c_str(), COLLISION_FILTER) == 0 )
			continue;

		if(DEBUG){std::cout << "Collision between[" << contacts->contact(i).collision1()
			     << "] and [" << contacts->contact(i).collision2() << "]\n";}


		/*
		/ TODO - Check if there is collision between the arm and object, then issue learning reward
		/ Task #6 Issue a reward based on collision between the arm and the object
        / Task #8 Issue a reward based on collision between the arm’s gripper base and the object.
        gripper: COLLISION_POINT
        can: COLLISION_ITEM
		*/
		if (GOAL == 1)
        {
		bool anyCollisionCheck = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0); // check any collision with the can
		if (anyCollisionCheck)
			{
				rewardHistory = REWARD_WIN;

				newReward  = true;
				endEpisode = true;

				return;
			}
		}
		else if (GOAL == 2)
        {
 		bool gripperCollisionCheck = (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT) == 0); // check any collision with the gripper base
		if (gripperCollisionCheck)
			{
				rewardHistory = REWARD_WIN;

				newReward  = true;
				endEpisode = true;

				return;
			}
		}
		else
			{
			if(DEBUG){printf("No valid goal set. \n GOAL 1: Any part of the robot arm touches the can. \n GOAL 2: The gripper base touches the can. \n");}
            }

	}
}


// upon recieving a new frame, update the AI agent
bool ArmPlugin::updateAgent()
{
	// convert uchar3 input from camera to planar BGR
	if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer[1], inputRawWidth, inputRawHeight,
							         inputState->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
	{
		printf("ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
			   inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

		return false;
	}

	// select the next action
	int action = 0;

	if( !agent->NextAction(inputState, &action) )
	{
		printf("ArmPlugin - failed to generate agent's next action\n");
		return false;
	}

	// make sure the selected action is in-bounds
	if( action < 0 || action >= DOF * 2 )
	{
		printf("ArmPlugin - agent selected invalid action, %i\n", action);
		return false;
	}

	if(DEBUG){printf("ArmPlugin - agent selected action %i\n", action);}



#if VELOCITY_CONTROL
	// if the action is even, increase the joint position by the delta parameter
	// if the action is odd,  decrease the joint position by the delta parameter


	/*
	/ TODO - Increase or decrease the joint velocity based on whether the action is even or odd
	/
	*/

	float velocity = 0.0; // TODO - Set joint velocity based on whether action is even or odd.

	if( velocity < VELOCITY_MIN )
		velocity = VELOCITY_MIN;

	if( velocity > VELOCITY_MAX )
		velocity = VELOCITY_MAX;

	vel[action/2] = velocity;

	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] += vel[n];

		if( ref[n] < JOINT_MIN )
		{
			ref[n] = JOINT_MIN;
			vel[n] = 0.0f;
		}
		else if( ref[n] > JOINT_MAX )
		{
			ref[n] = JOINT_MAX;
			vel[n] = 0.0f;
		}
	}
#else

	/*
	/ TODO - Increase or decrease the joint position based on whether the action is even or odd
	/
	*/
    // arbitrary rule to decide on how the action array translates to movement increase or decrease

  // Set joint position based on whether action is even or odd	
  float joint = 0.0; 
    if ( action % 2 == 0)
    {
        if(DEBUG){printf("action is even \n");}
        joint = ref[action/2] + actionJointDelta;
    }
    else
    {
        if(DEBUG){printf("action is odd \n");}
        joint = ref[action/2] - actionJointDelta;
    }

    if(DEBUG){printf("Joint Position: %f\n", joint);}


	// limit the joint to the specified range
	if( joint < JOINT_MIN )
		joint = JOINT_MIN;

	if( joint > JOINT_MAX )
		joint = JOINT_MAX;

	ref[action/2] = joint;

#endif

	return true;
}


// update joint reference positions, returns true if positions have been modified
bool ArmPlugin::updateJoints()
{
	if( testAnimation )	// test sequence
	{
		const float step = (JOINT_MAX - JOINT_MIN) * (float(1.0f) / float(ANIMATION_STEPS));

#if 0
		// range of motion
		if( animationStep < ANIMATION_STEPS )
		{
			animationStep++;
			printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MIN + step * float(animationStep);
		}
		else if( animationStep < ANIMATION_STEPS * 2 )
		{
			animationStep++;
			printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MAX - step * float(animationStep-ANIMATION_STEPS);
		}
		else
		{
			animationStep = 0;

		}

#else
		// return to base position
		for( uint32_t n=0; n < DOF; n++ )
		{

			if( ref[n] < resetPos[n] )
				ref[n] += step;
			else if( ref[n] > resetPos[n] )
				ref[n] -= step;

			if( ref[n] < JOINT_MIN )
				ref[n] = JOINT_MIN;
			else if( ref[n] > JOINT_MAX )
				ref[n] = JOINT_MAX;

		}

		animationStep++;
#endif

		// reset and loop the animation
		if( animationStep > ANIMATION_STEPS )
		{
			animationStep = 0;

			if( !loopAnimation )
				testAnimation = false;
		}
		else if( animationStep == ANIMATION_STEPS / 2 )
		{
			ResetPropDynamics();
		}

		return true;
	}

	else if( newState && agent != NULL )
	{
		// update the AI agent when new camera frame is ready
		episodeFrames++;

		if(DEBUG){printf("episode frame = %i\n", episodeFrames);}

		// reset camera ready flag
		newState = false;

		if( updateAgent() )
			return true;
	}

	return false;
}


// get the servo center for a particular degree of freedom
float ArmPlugin::resetPosition( uint32_t dof )
{
	return resetPos[dof];
}


// compute the distance between two bounding boxes
static float BoxDistance(const math::Box& a, const math::Box& b)
{
	float sqrDist = 0;

	if( b.max.x < a.min.x )
	{
		float d = b.max.x - a.min.x;
		sqrDist += d * d;
	}
	else if( b.min.x > a.max.x )
	{
		float d = b.min.x - a.max.x;
		sqrDist += d * d;
	}

	if( b.max.y < a.min.y )
	{
		float d = b.max.y - a.min.y;
		sqrDist += d * d;
	}
	else if( b.min.y > a.max.y )
	{
		float d = b.min.y - a.max.y;
		sqrDist += d * d;
	}

	if( b.max.z < a.min.z )
	{
		float d = b.max.z - a.min.z;
		sqrDist += d * d;
	}
	else if( b.min.z > a.max.z )
	{
		float d = b.min.z - a.max.z;
		sqrDist += d * d;
	}

	return sqrtf(sqrDist);
}


// called by the world update start event
void ArmPlugin::OnUpdate(const common::UpdateInfo& updateInfo)
{
	// deferred loading of the agent (this is to prevent Gazebo black/frozen display)
	if( !agent && updateInfo.simTime.Float() > 1.5f )
	{
		if( !createAgent() )
			return;
	}

	// verify that the agent is loaded
	if( !agent )
		return;

	// determine if we have new camera state and need to update the agent
	const bool hadNewState = newState && !testAnimation;

	// update the robot positions with vision/DQN
	if( updateJoints() )
	{
		double angle(1);

#if LOCKBASE
		j2_controller->SetJointPosition(this->model->GetJoint("base"), 	0);
		j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[0]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[1]);

#else
		j2_controller->SetJointPosition(this->model->GetJoint("base"), 	 ref[0]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[1]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[2]);
#endif
	}

	// episode timeout
	if( maxEpisodeLength > 0 && episodeFrames > maxEpisodeLength )
	{
		printf("ArmPlugin - triggering EOE, episode has exceeded %i frames\n", maxEpisodeLength);
		rewardHistory = REWARD_LOSS;
		newReward     = true;
		endEpisode    = true;
	}

	// if an EOE reward hasn't already been issued, compute an intermediary reward
	if( hadNewState && !newReward )
	{
		PropPlugin* prop = GetPropByName(PROP_NAME);

		if( !prop )
		{
			printf("ArmPlugin - failed to find Prop '%s'\n", PROP_NAME);
			return;
		}

		// get the bounding box for the prop object
		const math::Box& propBBox = prop->model->GetBoundingBox();
		physics::LinkPtr gripper  = model->GetLink(GRIP_NAME);

		if( !gripper )
		{
			printf("ArmPlugin - failed to find Gripper '%s'\n", GRIP_NAME);
			return;
		}

		// get the bounding box for the gripper
		const math::Box& gripBBox = gripper->GetBoundingBox();
		const float groundContact = 0.05f;

		/*
		/ TODO - set appropriate Reward for robot hitting the ground.
		/
		*/
		bool checkGroundContact = (gripBBox.min.z <= groundContact) ? true : false;
		// check if the gripper is hitting the ground or not using its bounding box attribute
		if(checkGroundContact)
		{

			if(DEBUG){printf("GROUND CONTACT, EOE\n");}

            // Reward for robot gripper hitting the ground
			rewardHistory = REWARD_LOSS; // Value of the previous reward
			newReward     = true;
			endEpisode    = true;
		}


		/*
		/ TODO - Issue an interim reward based on the distance to the object
		/
		*/


		if(!checkGroundContact)
		{
			const float distGoal = BoxDistance(gripBBox, propBBox); // compute the distance between two bounding boxes

			if(DEBUG){printf("distance('%s', '%s') = %f\n", gripper->GetName().c_str(), prop->model->GetName().c_str(), distGoal);}


			if( episodeFrames > 1 )
			{
				const float distDelta  = lastGoalDistance - distGoal;
				const float maxDistGoal = (maxDistGoal >= distGoal) ? maxDistGoal : distGoal; // get max distance to goal
				// compute the smoothed moving average of the delta of the distance to the goal
				// avgGoalDelta  = (avgGoalDelta * ALPHA) + (distDelta * (1 - ALPHA)); // recommended smoothed moving average formula
                
		        // check if the gripper is overshooting the can or not using its bounding box attribute
                // bool checkOvershoot = (gripBBox.max.x >= propBBox.max.x) ? true : false; // in Gazebo the red marker represents the X axis
                // rewardHistory = (checkOvershoot) ? REWARD_LOSS*0.5 : 0;
                // newReward     = true;
				// Compute reward based on distance from goal
				if (distGoal < lastGoalDistance)
                {  // is closer to goal
					// if (distGoal > groundContact)
                    // {   // has not reached goal
						rewardHistory = REWARD_WIN * ((maxDistGoal - distGoal) / maxDistGoal); // the closer the higher the reward
						newReward     = true;
						endEpisode    = false;
					// }
					// else if (distGoal <= groundContact)
                    // {  // has reached Goal
					// 	rewardHistory = REWARD_WIN;
					// 	newReward     = true;
					//	endEpisode    = true;
					// }
				}
				else if (distGoal >= lastGoalDistance)
                { // is farther away from goal
						rewardHistory = REWARD_LOSS;
						newReward     = true;
						endEpisode    = false;
                }
			}
			lastGoalDistance = distGoal;
		}
	}

	// issue rewards and train DQN
	if( newReward && agent != NULL )
	{
		if(DEBUG){printf("ArmPlugin - issuing reward %f, EOE=%s  %s\n", rewardHistory, endEpisode ? "true" : "false", (rewardHistory > 0.1f) ? "POS+" :(rewardHistory > 0.0f) ? "POS" : (rewardHistory < 0.0f) ? "    NEG" : "       ZERO");}
		agent->NextReward(rewardHistory, endEpisode);

		// reset reward indicator
		newReward = false;

		// reset for next episode
		if( endEpisode )
		{
			testAnimation    = true;	// reset the robot to base position
			loopAnimation    = false;
			endEpisode       = false;
			episodeFrames    = 0;
			lastGoalDistance = 0.0f;
			avgGoalDelta     = 0.0f;

			// track the number of wins and agent accuracy
			if( rewardHistory >= REWARD_WIN )
				successfulGrabs++;

			totalRuns++;
			printf("Current Accuracy:  %0.4f (%03u of %03u)  (reward=%+0.2f %s)\n", float(successfulGrabs)/float(totalRuns), successfulGrabs, totalRuns, rewardHistory, (rewardHistory >= REWARD_WIN ? "WIN" : "LOSS"));


			for( uint32_t n=0; n < DOF; n++ )
				vel[n] = 0.0f;
		}
	}
}

}
