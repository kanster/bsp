#include "pr2-sim.h"

/**
 * PR2 Constructors/Destructors
 */

PR2::PR2(bool view) {
	std::string working_dir = boost::filesystem::current_path().normalize().string();
	std::string bsp_dir = working_dir.substr(0,working_dir.find("bsp"));
	std::string env_file = bsp_dir + "bsp/pr2/envs/pr2-test.env.xml";

	std::string robot_name = "Brett";

	this->init(env_file, robot_name, view);
}

PR2::PR2(std::string env_file, std::string robot_name, bool view) {
	this->init(env_file, robot_name, view);
}

PR2::~PR2() {
	free(larm);
	free(rarm);
	free(head);
	free(hcam);
	free(lcam);
	free(rcam);
	env->Destroy();
}

/**
 * PR2 Initializers
 */

void SetViewer(rave::EnvironmentBasePtr penv, const std::string& viewername)
{
    rave::ViewerBasePtr viewer = RaveCreateViewer(penv,viewername);
    BOOST_ASSERT(!!viewer);

    // attach it to the environment:
    penv->Add(viewer);

    // finally call the viewer's infinite loop (this is why a separate thread is needed)
    bool showgui = true;
    viewer->main(showgui);
}

void PR2::init(std::string env_file, std::string robot_name, bool view) {
	RAVELOG_INFO("Initializing OpenRAVE\n");
	rave::RaveInitialize(true, rave::Level_Info);
	env = rave::RaveCreateEnvironment();
	RAVELOG_INFO("Loading environment: " + env_file + "\n");
	env->Load(env_file);

	robot = env->GetRobot(robot_name);

	if (view) {
		viewer_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(SetViewer, env, "qtcoin")));
	}

	larm = new Arm(robot, Arm::ArmType::left);
	rarm = new Arm(robot, Arm::ArmType::right);
	head = new Head(robot);

	hcam = new Camera(robot, "head_cam", 5);
	rcam = new Camera(robot, "r_gripper_cam", 5);
	lcam = new Camera(robot, "l_gripper_cam", 5);
}


Arm::Arm(rave::RobotBasePtr robot, ArmType arm_type) {
	this->robot = robot;
	this->arm_type = arm_type;

	if (arm_type == ArmType::left) {
		manip_name = "leftarm";
	} else {
		manip_name = "rightarm";
	}

	manip = robot->GetManipulator(manip_name);
	joint_indices = manip->GetArmIndices();
	num_joints = joint_indices.size();

	std::vector<double> lower_vec(num_joints), upper_vec(num_joints);
	robot->GetDOFLimits(lower_vec, upper_vec, joint_indices);

	lower = Matrix<double,ARM_DIM,1>(lower_vec.data());
	upper = Matrix<double,ARM_DIM,1>(upper_vec.data());
}

/**
 * Arm public methods
 */

Matrix<double,ARM_DIM,1> Arm::get_joint_values() {
	std::vector<double> joint_values;
	robot->GetDOFValues(joint_values, joint_indices);
	return Matrix<double,ARM_DIM,1>(joint_values.data());
}

void Arm::get_limits(Matrix<double,ARM_DIM,1>& l, Matrix<double,ARM_DIM,1>& u) {
	l = lower;
	u = upper;
}

rave::Transform Arm::get_pose() {
	return manip->GetEndEffectorTransform();
}

void Arm::set_joint_values(const Matrix<double,ARM_DIM,1>& j) {
	std::vector<double> joint_values_vec(ARM_DIM);
	for(int i=0; i < num_joints; ++i) {
		joint_values_vec[i] = std::min(j(i), upper(i));
		joint_values_vec[i] = std::max(j(i), lower(i));
	}

	robot->SetDOFValues(joint_values_vec, rave::KinBody::CheckLimitsAction::CLA_Nothing, joint_indices);
}

void Arm::set_pose(const rave::Transform& pose, std::string ref_frame) {
	std::vector<double> joint_values_vec(num_joints);
	rave_utils::cart_to_joint(manip, pose, ref_frame, "end_effector", joint_values_vec);

	Matrix<double,ARM_DIM,1> joint_values(joint_values_vec.data());
	set_joint_values(joint_values);
}

void Arm::set_posture(Posture posture) {
	std::vector<double> j;

	// joint values defined for left arm
	switch(posture) {
	case Posture::untucked:
		j = {0.4,  1.0,   0.0,  -2.05,  0.0,  -0.1,  0.0};
		break;
	case Posture::tucked:
		j = {0.06, 1.25, 1.79, -1.68, -1.73, -0.10, -0.09};
		break;
	case Posture::up:
		j = {0.33, -0.35,  2.59, -0.15,  0.59, -1.41, -0.27};
		break;
	case Posture::side:
		j = {1.832,  -0.332,   1.011,  -1.437,   1.1  ,  -2.106,  3.074};
		break;
	case Posture::mantis:
		j = {2.03018192, -0.05474993, 1.011, -1.47618716,  0.55995636, -1.42855926,  3.96467305};
		break;
	default:
		j = {0, 0, 0, 0, 0, 0, 0};
		break;
	}

	if (arm_type == ArmType::right) {
		j = {-j[0], j[1], -j[2], j[3], -j[4], j[5], -j[6]};
	}

	Matrix<double,ARM_DIM,1> j_vec(j.data());
	set_joint_values(j_vec);
}

void Arm::teleop() {
	double pos_step = .01;
	std::map<int,rave::Vector> delta_position =
	{
			{'a' , rave::Vector(0, pos_step, 0)},
			{'d' , rave::Vector(0, -pos_step, 0)},
			{'w' , rave::Vector(pos_step, 0, 0)},
			{'x' , rave::Vector(-pos_step, 0, 0)},
			{'+' , rave::Vector(0, 0, pos_step)},
			{'-' , rave::Vector(0, 0, -pos_step)},
	};

	double angle_step = 2.0*(M_PI/180);
	std::map<int,rave::Vector> delta_angle =
	{
			{'p', rave::Vector(angle_step, 0, 0)},
			{'o', rave::Vector(-angle_step, 0, 0)},
			{'k', rave::Vector(0, angle_step, 0)},
			{'l', rave::Vector(0, -angle_step, 0)},
			{'n', rave::Vector(0, 0, angle_step)},
			{'m', rave::Vector(0, 0, -angle_step)},
	};

	std::cout << manip_name << " teleop\n";

	char c;
	while ((c = utils::getch()) != 'q') {

		rave::Transform pose = get_pose();
		if (delta_position.count(c) > 0) {
			pose.trans += delta_position[c];
		} else if (delta_angle.count(c) > 0) {
			pose.rot = rave::geometry::quatFromAxisAngle(rave::geometry::axisAngleFromQuat(pose.rot) + delta_angle[c]);
		}

		set_pose(pose);
	}

	std::cout << manip_name << " end teleop\n";
}



/**
 * Head constructors
 */

Head::Head(rave::RobotBasePtr robot) {
	this->robot = robot;

	std::vector<std::string> joint_names = {"head_pan_joint", "head_tilt_joint"};
	for(int i=0; i < joint_names.size(); ++i) {
		joint_indices.push_back(robot->GetJointIndex(joint_names[i]));
	}
	num_joints = joint_indices.size();

	pose_link = robot->GetLink("wide_stereo_link");

	std::vector<double> lower_vec(num_joints), upper_vec(num_joints);
	robot->GetDOFLimits(lower_vec, upper_vec, joint_indices);

	lower = Matrix<double,HEAD_DIM,1>(lower_vec.data());
	upper = Matrix<double,HEAD_DIM,1>(upper_vec.data());
}

/**
 * Head public methods
 */

Matrix<double,HEAD_DIM,1> Head::get_joint_values() {
	std::vector<double> joint_values;
	robot->GetDOFValues(joint_values, joint_indices);
	return Matrix<double,HEAD_DIM,1>(joint_values.data());
}

void Head::get_limits(Matrix<double,HEAD_DIM,1>& l, Matrix<double,HEAD_DIM,1>& u) {
	l = lower;
	u = upper;
}

rave::Transform Head::get_pose() {
	return pose_link->GetTransform();
}

void Head::set_joint_values(Matrix<double,HEAD_DIM,1>& j) {
	std::vector<double> joint_values_vec(HEAD_DIM);
	for(int i=0; i < num_joints; ++i) {
		joint_values_vec[i] = std::min(j(i), upper(i));
		joint_values_vec[i] = std::max(j(i), lower(i));
	}

	robot->SetDOFValues(joint_values_vec, rave::KinBody::CheckLimitsAction::CLA_Nothing, joint_indices);
}

void Head::look_at(const rave::Transform &pose, const std::string ref_frame) {
	rave::Transform world_from_ref, world_from_cam, ref_from_cam;

	if (ref_frame == "world") {
		world_from_ref.identity();
	} else {
		world_from_ref = robot->GetLink(ref_frame)->GetTransform();
	}
	world_from_cam = get_pose();
	ref_from_cam = world_from_ref.inverse()*world_from_cam;

	rave::Vector ax = pose.trans - ref_from_cam.trans;
	double pan = atan(ax.y/ax.x);
	double tilt = asin(-ax.z/sqrt(ax.lengthsqr3()));

	Matrix<double,HEAD_DIM,1> joint_values(pan, tilt);
	set_joint_values(joint_values);
}

void Head::teleop() {
	double pos_step = .01;
	std::map<int,std::vector<double> > delta_joints =
	{
			{'a' , {pos_step, 0}},
			{'d' , {-pos_step, 0}},
			{'w' , {0, -pos_step}},
			{'x' , {0, pos_step}},
	};

	std::cout << "Head teleop\n";

	char c;
	while ((c = utils::getch()) != 'q') {

		Matrix<double,HEAD_DIM,1> j = get_joint_values();
		if (delta_joints.count(c) > 0) {
			j += Matrix<double,HEAD_DIM,1>(delta_joints[c].data());
		}

		set_joint_values(j);

	}

	std::cout << "Head end teleop\n";
}

/**
 * Camera constructors
 */

Camera::Camera(rave::RobotBasePtr r, std::string camera_name, double mr) : robot(r), max_range(mr) {
	bool found_sensor = false;
	std::vector<rave::RobotBase::AttachedSensorPtr> sensors = robot->GetAttachedSensors();
	for(int i=0; i < sensors.size(); ++i) {
		if (sensors[i]->GetName() == camera_name) {
			sensor = sensors[i]->GetSensor();
			found_sensor = true;
			break;
		}
	}

	assert(found_sensor);

	rave::SensorBase::SensorGeometryPtr geom = sensor->GetSensorGeometry(rave::SensorBase::ST_Camera);

	boost::shared_ptr<rave::SensorBase::CameraGeomData> cam_geom =
			boost::static_pointer_cast<rave::SensorBase::CameraGeomData>(geom);

	height = cam_geom->height;
	width = cam_geom->width;
	f = cam_geom->KK.fx;
	F = cam_geom->KK.focal_length;

	H = F*(height/f);
	W = F*(width/f);
}

/**
 * Camera public methods
 */

Matrix<double,N_SUB,3> Camera::get_directions() {
	Matrix<double,H_SUB,W_SUB> height_grid = Matrix<double,H_SUB,1>::LinSpaced(H_SUB, -H/2.0, H/2.0).replicate(1,W_SUB);
	Matrix<double,H_SUB,W_SUB> width_grid = Matrix<double,1,W_SUB>::LinSpaced(W_SUB, -W/2.0, W/2.0).replicate(H_SUB,1);

	Matrix<double,N_SUB,1> height_grid_vec(height_grid.data());
	Matrix<double,N_SUB,1> width_grid_vec(width_grid.data());
	Matrix<double,N_SUB,1> z_grid = Matrix<double,N_SUB,1>::Zero();

	Matrix<double,N_SUB,3> offsets;
	offsets << width_grid_vec, height_grid_vec, z_grid;

	Matrix<double,N_SUB,3> points_cam = RowVector3d(0,0,max_range).replicate(N_SUB,1) + (max_range/F)*offsets;

	Matrix4d ref_from_world = rave_utils::rave_to_eigen(sensor->GetTransform());
	Vector3d origin_world_pos = ref_from_world.block<3,1>(0,3);

	Matrix<double,N_SUB,3> directions;

	Matrix4d point_cam = Matrix4d::Identity();
	Vector3d point_world;
	for(int i=0; i < N_SUB; ++i) {
		point_cam.block<3,1>(0,3) = points_cam.row(i);
		point_world = (ref_from_world*point_cam).block<3,1>(0,3);

		directions.row(i) = point_world - origin_world_pos;
	}

	return directions;
}

std::vector<std::vector<Beam3d> > Camera::get_beams() {
	std::vector<std::vector<Beam3d> > beams(H_SUB-1, std::vector<Beam3d>(W_SUB-1));

	RowVector3d origin_pos = rave_utils::rave_to_eigen(sensor->GetTransform().trans);

	Matrix<double,N_SUB,3> dirs = get_directions();

	Matrix<double,N_SUB,3> hits;

	rave::EnvironmentBasePtr env = robot->GetEnv();
	rave::RAY ray;
	ray.pos = sensor->GetTransform().trans;
	rave::CollisionReportPtr report(new rave::CollisionReport());
	for(int i=0; i < N_SUB; ++i) {
		ray.dir.x = dirs(i,0);
		ray.dir.y = dirs(i,1);
		ray.dir.z = dirs(i,2);
		if (env->CheckCollision(ray, report)) {
			hits.row(i) = rave_utils::rave_to_eigen(report->contacts[0].pos);
		} else {
			hits.row(i) = origin_pos + (max_range / sqrt(ray.dir.lengthsqr2()))*dirs.row(i);
		}
	}

	for(int j=0; j < W_SUB-1; ++j) {
		for(int i=0; i < H_SUB-1; ++i) {
			beams[i][j].base = origin_pos;
			beams[i][j].a = hits.row((j+1)*H_SUB+i);
			beams[i][j].b = hits.row(j*H_SUB+i);
			beams[i][j].c = hits.row(j*H_SUB+i+1);
			beams[i][j].d = hits.row((j+1)*H_SUB+i+1);
		}
	}

	return beams;
}

std::vector<Triangle3d> Camera::get_border(const std::vector<std::vector<Beam3d> >& beams) {
	std::vector<Triangle3d> border;

	int rows = beams.size(), cols = beams[0].size();

	// deal with left and right border columns
	for(int i=0; i < rows; ++i) {
		border.push_back(Triangle3d(beams[i][0].base, beams[i][0].b, beams[i][0].c));
		border.push_back(Triangle3d(beams[i][cols-1].base, beams[i][cols-1].a, beams[i][cols-1].d));
	}

	// deal with top and bottom border rows
	for(int j=0; j < cols; ++j) {
		border.push_back(Triangle3d(beams[0][j].base, beams[0][j].a, beams[0][j].b));
		border.push_back(Triangle3d(beams[rows-1][j].base, beams[rows-1][j].c, beams[rows-1][j].d));
	}

	// connect with left and top
	for(int i=0; i < rows; ++i) {
		for(int j=0; j < cols; ++j) {
			// left
			if (i > 0) {
				border.push_back(Triangle3d(beams[i-1][j].a, beams[i-1][j].d, beams[i][j].b));
				border.push_back(Triangle3d(beams[i-1][j].b, beams[i][j].b, beams[i][j].c));
			}

			if (j > 0) {
				border.push_back(Triangle3d(beams[i][j-1].c, beams[i][j-1].d, beams[i][j].b));
				border.push_back(Triangle3d(beams[i][j-1].b, beams[i][j].b, beams[i][j].a));
			}
		}
	}

	std::vector<Triangle3d> pruned_border;
	for(int i=0; i < border.size(); ++i) {
		if (border[i].area() > epsilon) {
			pruned_border.push_back(border[i]);
		}
	}

	return pruned_border;
}

bool Camera::is_inside(const Vector3d& p, std::vector<std::vector<Beam3d> >& beams) {
	bool inside = false;
	for(int i=0; i < beams.size(); ++i) {
		for(int j=0; j < beams[i].size(); ++j) {
			if (beams[i][j].is_inside(p)) {
				inside = true;
				break;
			}
		}
		if (inside) { break; }
	}

	return inside;
}

double Camera::signed_distance(const Vector3d& p, std::vector<std::vector<Beam3d> >& beams, std::vector<Triangle3d>& border) {
	double sd_sign = (is_inside(p, beams)) ? -1 : 1;

	double sd = INFINITY;
	for(int i=0; i < border.size(); ++i) {
		sd = std::min(sd, border[i].distance_to(p));
	}

	return (sd_sign*sd);
}

void Camera::plot_fov(std::vector<std::vector<Beam3d> >& beams) {
	Vector3d color(0,1,0);
	// plot the ends
	for(int i=0; i < beams.size(); ++i) {
		for(int j=0; j < beams[i].size(); ++j) {
			beams[i][j].plot(sensor->GetEnv());
		}
	}

	int rows = beams.size(), cols = beams[0].size();
	// plot the left and right
	for(int i=0; i < rows; ++i) {
		rave_utils::plot_segment(sensor->GetEnv(), beams[i][0].base, beams[i][0].b, color);
		rave_utils::plot_segment(sensor->GetEnv(), beams[i][0].base, beams[i][0].c, color);

		rave_utils::plot_segment(sensor->GetEnv(), beams[i][cols-1].base, beams[i][cols-1].a, color);
		rave_utils::plot_segment(sensor->GetEnv(), beams[i][cols-1].base, beams[i][cols-1].d, color);
	}
	// plot the top and bottom
	for(int j=0; j < cols; ++j) {
		rave_utils::plot_segment(sensor->GetEnv(), beams[0][j].base, beams[0][j].a, color);
		rave_utils::plot_segment(sensor->GetEnv(), beams[0][j].base, beams[0][j].b, color);

		rave_utils::plot_segment(sensor->GetEnv(), beams[rows-1][j].base, beams[rows-1][j].c, color);
		rave_utils::plot_segment(sensor->GetEnv(), beams[rows-1][j].base, beams[rows-1][j].d, color);
	}
}