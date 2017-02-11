/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Ken Anderson
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ken Anderson */

/*
  Code to time parameterize a trajectory into a cubic spline
  while respecting velocity, acceleration, and jerk constraints.
*/

#include <moveit/trajectory_processing/iterative_spline_parameterization.h>
#include <moveit_msgs/JointLimits.h>
#include <console_bridge/console.h>
#include <moveit/robot_state/conversions.h>
#include <vector>

#define VLIMIT 1.0  // default
#define ALIMIT 3.0  // default
#define JLIMIT 9.0  // default

/*
  Comment-out to disable jerk limits.
  This would be the case if you want to allow bang-bang style control,
  where the controller can go from min acceleration to max acceleration
  instantaneously.  In other words, the acceleration curve is discontinuous.
  Since the acceleration can change instantly, matching the initial and final
  acceleration is unneccessary, as is adding and moving the two extra points.
  This will get the shortest paths and should be comparable to 'optimal'
  methods that have a trapazoidal velocity curve.

  However, fitting a cubic spline creates a trajectory with a continuous
  acceleration curve.
  If you are using this technique, then you probably don't want rapid changes
  in acceleration like a bang-bang controller.
  This means that jerks limits need to be enforced.
  The downside is that if the limits are low, you will notice visible oscilation
  around what you may expect as the 'optimal' velocity curve.
  Increasing the jerk limits will reduce oscilation and speed up execution.
  By default, this should be ENABLED.
*/
#define ENABLE_JERK

namespace trajectory_processing
{
static void fit_cubic_spline(const int n, const double dt[], const double x[], double x1[], double x2[]);
static void adjust_two_positions(const int n, const double dt[], double x[], double x1[], double x2[],
                                 const double x2_i, const double x2_f);
static void init_times(const int n, double dt[], const double x[], const double max_velocity);
static int fit_spline_and_adjust_times(const int n, double dt[], const double x[], double x1[], double x2[],
                                       const double vlimit, const double alimit, const double jlimit,
                                       const double tfactor);

// The path of a single joint: positions, velocities, and accelerations
struct SingleJointTrajectory
{
  std::vector<double> positions;  // joint's position at time[x]
  std::vector<double> velocities;
  std::vector<double> accelerations;
  double initial_velocity;
  double final_velocity;
  double initial_acceleration;
  double final_acceleration;
  double max_velocity;
  double max_acceleration;
  double max_jerk;
};

IterativeSplineParameterization::IterativeSplineParameterization(double max_time_change_per_it, bool add_points)
  : max_time_change_per_it_(1.0 + max_time_change_per_it), add_points_(add_points)
{
}

IterativeSplineParameterization::~IterativeSplineParameterization()
{
}

bool IterativeSplineParameterization::computeTimeStamps(robot_trajectory::RobotTrajectory& trajectory,
                                                        const double max_velocity_scaling_factor,
                                                        const double max_acceleration_scaling_factor) const
{
  if (trajectory.empty())
    return true;

  const robot_model::JointModelGroup* group = trajectory.getGroup();
  if (!group)
  {
    logError("It looks like the planner did not set the group the plan was computed for");
    return false;
  }
  const robot_model::RobotModel& rmodel = group->getParentModel();
  const std::vector<int>& idx = group->getVariableIndexList();
  const std::vector<std::string>& vars = group->getVariableNames();
  double acceleration_scaling_factor = 1.0;
  double velocity_scaling_factor = 1.0;
  unsigned num_points = trajectory.getWayPointCount();
  unsigned num_joints = group->getVariableCount();

  // Set scaling factors
  if (max_acceleration_scaling_factor > 0.0 && max_acceleration_scaling_factor <= 1.0)
    acceleration_scaling_factor = max_acceleration_scaling_factor;
  else if (max_acceleration_scaling_factor == 0.0)
    logDebug("A max_acceleration_scaling_factor of 0.0 was specified, defaulting to %f instead.",
             acceleration_scaling_factor);
  else
    logWarn("Invalid max_acceleration_scaling_factor %f specified, defaulting to %f instead.",
            max_acceleration_scaling_factor, acceleration_scaling_factor);

  if (max_velocity_scaling_factor > 0.0 && max_velocity_scaling_factor <= 1.0)
    velocity_scaling_factor = max_velocity_scaling_factor;
  else if (max_velocity_scaling_factor == 0.0)
    logDebug("A max_velocity_scaling_factor of 0.0 was specified, defaulting to %f instead.", velocity_scaling_factor);
  else
    logWarn("Invalid max_velocity_scaling_factor %f specified, defaulting to %f instead.", max_velocity_scaling_factor,
            velocity_scaling_factor);

  // No wrapped angles.
  trajectory.unwind();

#ifdef ENABLE_JERK
  // Insert 2nd and 2nd-last points
  // (required to force acceleration to specified values at endpoints)
  if (add_points_ && trajectory.getWayPointCount() >= 2)
  {
    robot_state::RobotState point = trajectory.getWayPoint(0);
    robot_state::RobotStatePtr p0, p1;

    p0 = trajectory.getWayPointPtr(0);
    p1 = trajectory.getWayPointPtr(1);
    for (unsigned j = 0; j < num_joints; j++)
    {
      point.setVariablePosition(idx[j],
                                (9 * p0->getVariablePosition(idx[j]) + 1 * p1->getVariablePosition(idx[j])) / 10);
      point.setVariableVelocity(idx[j],
                                (9 * p0->getVariableVelocity(idx[j]) + 1 * p1->getVariableVelocity(idx[j])) / 10);
      point.setVariableAcceleration(
          idx[j], (9 * p0->getVariableAcceleration(idx[j]) + 1 * p1->getVariableAcceleration(idx[j])) / 10);
    }
    trajectory.insertWayPoint(1, point, 0.0);
    num_points++;

    p0 = trajectory.getWayPointPtr(num_points - 2);
    p1 = trajectory.getWayPointPtr(num_points - 1);
    for (unsigned j = 0; j < num_joints; j++)
    {
      point.setVariablePosition(idx[j],
                                (1 * p0->getVariablePosition(idx[j]) + 9 * p1->getVariablePosition(idx[j])) / 10);
      point.setVariableVelocity(idx[j],
                                (1 * p0->getVariableVelocity(idx[j]) + 9 * p1->getVariableVelocity(idx[j])) / 10);
      point.setVariableAcceleration(
          idx[j], (1 * p0->getVariableAcceleration(idx[j]) + 9 * p1->getVariableAcceleration(idx[j])) / 10);
    }
    trajectory.insertWayPoint(num_points - 1, point, 0.0);
    num_points++;
  }
#endif

  // JointTrajectory indexes in [point][joint] order.
  // We need [joint][point] order to solve efficiently,
  // so convert form here.

  std::vector<SingleJointTrajectory> t2(num_joints);

  for (unsigned j = 0; j < num_joints; j++)
  {
    const robot_model::VariableBounds& bounds = rmodel.getVariableBounds(vars[j]);

    t2[j].positions.resize(num_points);
    t2[j].velocities.resize(num_points);
    t2[j].accelerations.resize(num_points);
    t2[j].initial_velocity = trajectory.getWayPointPtr(0)->getVariableVelocity(idx[j]);
    t2[j].final_velocity = trajectory.getWayPointPtr(num_points - 1)->getVariableVelocity(idx[j]);
    t2[j].initial_acceleration = trajectory.getWayPointPtr(0)->getVariableAcceleration(idx[j]);
    t2[j].final_acceleration = trajectory.getWayPointPtr(num_points - 1)->getVariableAcceleration(idx[j]);

    t2[j].max_velocity = VLIMIT;
    if (bounds.velocity_bounded_)
      t2[j].max_velocity = std::min(fabs(bounds.max_velocity_), fabs(bounds.min_velocity_));
    t2[j].max_velocity *= velocity_scaling_factor;

    t2[j].max_acceleration = ALIMIT;
    if (bounds.acceleration_bounded_)
      t2[j].max_acceleration = std::min(fabs(bounds.max_acceleration_), fabs(bounds.min_acceleration_));
    t2[j].max_acceleration *= acceleration_scaling_factor;

#ifdef ENABLE_JERK
    t2[j].max_jerk = JLIMIT;
#else
    t2[j].max_jerk = std::numeric_limits<double>::max();
#endif
  }

  for (unsigned i = 0; i < num_points; i++)
  {
    for (unsigned j = 0; j < num_joints; j++)
    {
      t2[j].positions[i] = trajectory.getWayPointPtr(i)->getVariablePosition(idx[j]);
      t2[j].velocities[i] = trajectory.getWayPointPtr(i)->getVariableVelocity(idx[j]);
      t2[j].accelerations[i] = trajectory.getWayPointPtr(i)->getVariableAcceleration(idx[j]);
    }
  }

  // Error check
  if (max_time_change_per_it_ <= 1.0)
  {
    printf("max time change is %f, needs to be higher than 1.0.\n", max_time_change_per_it_);
    return false;
  }
  if (num_points < 4)
  {
    printf("number of waypoints %d, needs to be greater than 3.\n", num_points);
    return false;
  }
  for (unsigned j = 0; j < num_joints; j++)
  {
    if (fabs(t2[j].velocities[0]) > t2[j].max_velocity)
    {
      printf("Initial velocity %f out of bounds\n", t2[j].velocities[0]);
      return false;
    }
    else if (fabs(t2[j].velocities[num_points - 1]) > t2[j].max_velocity)
    {
      printf("Final velocity %f out of bounds\n", t2[j].velocities[num_points - 1]);
      return false;
    }
    else if (fabs(t2[j].accelerations[0]) > t2[j].max_acceleration)
    {
      printf("Initial acceleration %f out of bounds\n", t2[j].accelerations[0]);
      return false;
    }
    else if (fabs(t2[j].accelerations[num_points - 1]) > t2[j].max_acceleration)
    {
      printf("Final acceleration %f out of bounds\n", t2[j].accelerations[num_points - 1]);
      return false;
    }
  }

  // Initiialize times
  // 0.01 to prevent divide-by-zero
  std::vector<double> time_diff(trajectory.getWayPointCount() - 1, 0.01);
  for (unsigned j = 0; j < num_joints; j++)
    init_times(num_points, &time_diff[0], &t2[j].positions[0], t2[j].max_velocity);

  // Fit initial spline (satisfies initial/final velocity)
  int loop = 1;
  while (loop)
  {
    loop = 0;
    for (unsigned j = 0; j < num_joints; j++)
    {
      while (fit_spline_and_adjust_times(num_points, &time_diff[0], &t2[j].positions[0], &t2[j].velocities[0],
                                         &t2[j].accelerations[0], t2[j].max_velocity, t2[j].max_acceleration,
                                         t2[j].max_jerk, max_time_change_per_it_))
        loop = 1;  // repeat until no adjustments
    }
  }

#ifdef ENABLE_JERK
  // Move points to satisfy initial/final acceleration
  loop = 1;
  while (loop)
  {
    loop = 0;
    for (unsigned j = 0; j < num_joints; j++)
    {
      adjust_two_positions(num_points, &time_diff[0], &t2[j].positions[0], &t2[j].velocities[0],
                           &t2[j].accelerations[0], t2[j].initial_acceleration, t2[j].final_acceleration);
      while (fit_spline_and_adjust_times(num_points, &time_diff[0], &t2[j].positions[0], &t2[j].velocities[0],
                                         &t2[j].accelerations[0], t2[j].max_velocity, t2[j].max_acceleration,
                                         t2[j].max_jerk, max_time_change_per_it_))
        loop = 1;  // repeat until no more adjustments
    }
  }
#endif

  // Convert back to JointTrajectory form
  for (unsigned i = 1; i < num_points; i++)
    trajectory.setWayPointDurationFromPrevious(i, time_diff[i - 1]);
  for (unsigned i = 0; i < num_points; i++)
  {
    for (unsigned j = 0; j < num_joints; j++)
    {
      trajectory.getWayPointPtr(i)->setVariablePosition(idx[j], t2[j].positions[i]);
      trajectory.getWayPointPtr(i)->setVariableVelocity(idx[j], t2[j].velocities[i]);
      trajectory.getWayPointPtr(i)->setVariableAcceleration(idx[j], t2[j].accelerations[i]);
    }
  }

  return true;
}

//////// Internal functions //////////////

/*
  Fit a 'clamped' cubic spline over a series of points.
  A cubic spline ensures continuous function across positions,
  1st derivative (velocities), and 2nd derivative (accelerations).
  'Clamped' means the first derivative at the endpoints is specified.

  Fitting a cubic spline involves solving a series of linear equations.
  The general form for each segment is:
    (tj-t_(j-1))*x"_(j-1) + 2*(t_(j+1)-t_(j-1))*x"j + (t_(j+1)-tj)*x"_j+1) =
          (x_(j+1)-xj)/(t_(j+1)-tj) - (xj-x_(j-1))/(tj-t_(j-1))
  And the first and last segment equations are clamped to specified values: x1_i and x1_f.

  Represented in matrix form:
  [ 2*(t1-t0)   (t1-t0)                              0              ][x0"]       [(x1-x0)/(t1-t0) - t1_i           ]
  [ t1-t0       2*(t2-t0)   t2-t1                                   ][x1"]       [(x2-x1)/(t2-t1) - (x1-x0)/(t1-t0)]
  [             t2-t1       2*(t3-t1)   t3-t2                       ][x2"] = 6 * [(x3-x2)/(t3/t2) - (x2-x1)/(t2-t1)]
  [                       ...         ...         ...               ][...]       [...                              ]
  [ 0                                    tN-t_(N-1)  2*(tN-t_(N-1)) ][xN"]       [t1_f - (xN-x_(N-1))/(tN-t_(N-1)) ]

  This matrix is tridiagonal, which can be solved solved in O(N) time
  using the tridiagonal algorithm.
  There is a forward propogation pass followed by a backsubstitution pass.

  n is the number of points
  dt contains the time difference between each point (size=n-1)
  x  contains the positions                          (size=n)
  x1 contains the 1st derivative (velocities)        (size=n)
     x1[0] and x1[n-1] MUST be specified.
  x2 contains the 2nd derivative (accelerations)     (size=n)
  x1 and x2 are filled in by the algorithm.
*/

static void fit_cubic_spline(const int n, const double dt[], const double x[], double x1[], double x2[])
{
  int i;
  const double x1_i = x1[0], x1_f = x1[n - 1];

  // Tridiagonal alg - forward sweep
  // x1 and x2 used to store the temporary coefficients c and d
  // (will get overwritten during backsubstitution)
  double *c = x1, *d = x2;
  c[0] = 0.5;
  d[0] = 3.0 * ((x[1] - x[0]) / dt[0] - x1_i) / dt[0];
  for (i = 1; i <= n - 2; i++)
  {
    const double dt2 = dt[i - 1] + dt[i];
    const double a = dt[i - 1] / dt2;
    const double denom = 2.0 - a * c[i - 1];
    c[i] = (1.0 - a) / denom;
    d[i] = 6.0 * ((x[i + 1] - x[i]) / dt[i] - (x[i] - x[i - 1]) / dt[i - 1]) / dt2;
    d[i] = (d[i] - a * d[i - 1]) / denom;
  }
  const double denom = dt[n - 2] * (2.0 - c[n - 2]);
  d[n - 1] = 6.0 * (x1_f - (x[n - 1] - x[n - 2]) / dt[n - 2]);
  d[n - 1] = (d[n - 1] - dt[n - 2] * d[n - 2]) / denom;

  // Tridiagonal alg - backsubstitution sweep
  // 2nd derivative
  x2[n - 1] = d[n - 1];
  for (i = n - 2; i >= 0; i--)
    x2[i] = d[i] - c[i] * x2[i + 1];

  // 1st derivative
  x1[0] = x1_i;
  for (i = 1; i < n - 1; i++)
    x1[i] = (x[i + 1] - x[i]) / dt[i] - (2 * x2[i] + x2[i + 1]) * dt[i] / 6.0;
  x1[n - 1] = x1_f;
}

/*
  Modify the value of x[1] and x[N-2]
  so that 2nd derivative starts and ends at specified value.
  This involves fitting the spline twice,
  then solving for the specified value.

  x2_i and x2_f are the (initial and final) 2nd derivative at 0 and N-1
*/

static void adjust_two_positions(const int n, const double dt[], double x[], double x1[], double x2[],
                                 const double x2_i, const double x2_f)
{
  // printf("Find Intersection\n");
  x[1] = x[0];
  x[n - 2] = x[n - 3];
  fit_cubic_spline(n, dt, x, x1, x2);
  double a0 = x2[0];
  double b0 = x2[n - 1];

  x[1] = x[2];
  x[n - 2] = x[n - 1];
  fit_cubic_spline(n, dt, x, x1, x2);
  double a2 = x2[0];
  double b2 = x2[n - 1];

  // we can solve this with linear equation (use two-point form)
  if (a2 != a0)
    x[1] = x[0] + ((x[2] - x[0]) / (a2 - a0)) * (x2_i - a0);
  if (b2 != b0)
    x[n - 2] = x[n - 3] + ((x[n - 1] - x[n - 3]) / (b2 - b0)) * (x2_f - b0);
}

/*
  Find time required to go max velocity on each segment.
  Increase a segment's time interval if the current time isn't long enough.
*/

static void init_times(const int n, double dt[], const double x[], const double max_velocity)
{
  int i;
  for (i = 0; i < n - 1; i++)
  {
    double time = fabs((x[i + 1] - x[i]) / max_velocity) + 0.001;
    if (dt[i] < time)
      dt[i] = time;
  }
}

/*
  Fit a spline, then check each interval to see if bounds are met.
  If all bounds met (no time adjustments made), return 0.
  If bounds not met (time adjustments made), slightly increase the
  surrounding time intervals and return 1.

  n is the number of points
  dt contains the time difference between each point (size=n-1)
  x  contains the positions                          (size=n)
  x1 contains the 1st derivative (velocities)        (size=n)
     x1[0] and x1[n-1] MUST be specified.
  x2 contains the 2nd derivative (accelerations)     (size=n)
  vlimit is the max velocity for this joint.
  alimit is the max acceleration for this joint.
  jlimit is the max jerk for this joint.
  tfactor is the time adjustment (multiplication) factor.
  x1 and x2 are filled in by the algorithm.
*/

static int fit_spline_and_adjust_times(const int n, double dt[], const double x[], double x1[], double x2[],
                                       const double vlimit, const double alimit, const double jlimit,
                                       const double tfactor)
{
  int i, ret = 0;

  fit_cubic_spline(n, dt, x, x1, x2);

  // Instantaneous velocity is calculated at each point
  for (i = 0; i < n - 1; i++)
  {
    const double vel = x1[i];
    const double vel2 = x1[i + 1];
    if (fabs(vel) > vlimit || fabs(vel2) > vlimit)
    {
      dt[i] *= tfactor;
      ret = 1;
    }
  }
  // Instantaneous acceleration is calculated at each point
  if (ret == 0)
  {
    for (i = 0; i < n - 1; i++)
    {
      const double acc = x2[i];
      const double acc2 = x2[i + 1];
      if (fabs(acc) > alimit || fabs(acc2) > alimit)
      {
        dt[i] *= tfactor;
        ret = 1;
      }
    }
  }

  // Jerk is discontinuous, but constant for each segment
  if (ret == 0)
  {
    for (i = 0; i < n - 1; i++)
    {
      const double jrk = (x2[i + 1] - x2[i]) / dt[i];
      if (fabs(jrk) > jlimit)
      {
        dt[i] *= tfactor;
        ret = 1;
      }
    }
  }

  return ret;
}
}
