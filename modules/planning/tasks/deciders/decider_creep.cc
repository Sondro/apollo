/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/tasks/deciders/decider_creep.h"

#include <string>

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/planning/common/planning_context.h"
#include "modules/planning/scenarios/util/util.h"

namespace apollo {
namespace planning {

using common::Status;
using hdmap::PathOverlap;

uint32_t DeciderCreep::creep_clear_counter_ = 0;

DeciderCreep::DeciderCreep(const TaskConfig& config) : Decider(config) {
  CHECK(config_.has_decider_creep_config());
  SetName("DeciderCreep");
}

Status DeciderCreep::Process(Frame* frame,
                             ReferenceLineInfo* reference_line_info) {
  CHECK_NOTNULL(frame);
  CHECK_NOTNULL(reference_line_info);

  double stop_line_s = 0.0;
  std::string stop_sign_overlap_id =
      PlanningContext::Planningstatus().stop_sign()
          .current_stop_sign_overlap_id();
  if (!stop_sign_overlap_id.empty()) {
    // refresh overlap along reference line
    PathOverlap* current_stop_sign_overlap =
        scenario::util::RefreshOverlapOnReferenceLine(
            *reference_line_info,
            stop_sign_overlap_id,
            ReferenceLineInfo::STOP_SIGN);
    if (current_stop_sign_overlap) {
      stop_line_s = current_stop_sign_overlap->end_s;
    }
  } else if (PlanningContext::GetScenarioInfo()
                 ->current_traffic_light_overlaps.size() > 0) {
    stop_line_s = PlanningContext::GetScenarioInfo()
                                  ->current_traffic_light_overlaps[0]
                                  .end_s;
  }
  if (stop_line_s > 0.0) {
    BuildStopDecision(stop_line_s, frame, reference_line_info);
  }

  return Status::OK();
}

double DeciderCreep::FindCreepDistance(
    const Frame& frame, const ReferenceLineInfo& reference_line_info) {
  // more delicate design of creep distance
  return 0.5;
}

// TODO(all): revisit & rewrite
bool DeciderCreep::BuildStopDecision(const double stop_line_s,
                                     Frame* frame,
                                     ReferenceLineInfo* reference_line_info) {
  CHECK_NOTNULL(frame);
  CHECK_NOTNULL(reference_line_info);

  double creep_stop_s =
      stop_line_s + FindCreepDistance(*frame, *reference_line_info);

  // create virtual stop wall
  std::string virtual_obstacle_id = CREEP_VO_ID_PREFIX + std::string("SS");
  auto* obstacle = frame->CreateStopObstacle(reference_line_info,
                                             virtual_obstacle_id, creep_stop_s);
  if (!obstacle) {
    AERROR << "Failed to create obstacle [" << virtual_obstacle_id << "]";
    return false;
  }
  Obstacle* stop_wall = reference_line_info->AddObstacle(obstacle);
  if (!stop_wall) {
    AERROR << "Failed to create obstacle for: " << virtual_obstacle_id;
    return false;
  }

  // build stop decision
  const double stop_distance = config_.decider_creep_config().stop_distance();
  const double stop_s = creep_stop_s - stop_distance;
  const auto& reference_line = reference_line_info->reference_line();
  auto stop_point = reference_line.GetReferencePoint(stop_s);
  double stop_heading = reference_line.GetReferencePoint(stop_s).heading();

  ObjectDecisionType stop;
  auto stop_decision = stop.mutable_stop();
  stop_decision->set_reason_code(StopReasonCode::STOP_REASON_CREEPER);
  stop_decision->set_distance_s(-stop_distance);
  stop_decision->set_stop_heading(stop_heading);
  stop_decision->mutable_stop_point()->set_x(stop_point.x());
  stop_decision->mutable_stop_point()->set_y(stop_point.y());
  stop_decision->mutable_stop_point()->set_z(0.0);

  auto* path_decision = reference_line_info->path_decision();
  path_decision->AddLongitudinalDecision("Creeper", stop_wall->Id(), stop);

  return true;
}

bool DeciderCreep::CheckCreepDone(const Frame& frame,
                                  const ReferenceLineInfo& reference_line_info,
                                  const double stop_sign_overlap_end_s,
                                  const double wait_time_sec,
                                  const double timeout_sec) {
  const auto& creep_config = config_.decider_creep_config();
  bool creep_done = false;
  double creep_stop_s =
      stop_sign_overlap_end_s + FindCreepDistance(frame, reference_line_info);

  const double distance =
      creep_stop_s - reference_line_info.AdcSlBoundary().end_s();
  if (distance < creep_config.max_valid_stop_distance() ||
      wait_time_sec >= timeout_sec) {
    bool all_far_away = true;
    for (auto* obstacle :
         reference_line_info.path_decision().obstacles().Items()) {
      if (obstacle->IsVirtual() || obstacle->IsStatic()) {
        continue;
      }
      if (obstacle->reference_line_st_boundary().min_t() <
          creep_config.min_boundary_t()) {
        const double kepsilon = 1e-6;
        double obstacle_traveled_s =
            obstacle->reference_line_st_boundary().bottom_left_point().s() -
            obstacle->reference_line_st_boundary().bottom_right_point().s();
        ADEBUG << "obstacle[" << obstacle->Id() << "] obstacle_st_min_t["
               << obstacle->reference_line_st_boundary().min_t()
               << "] obstacle_st_min_s["
               << obstacle->reference_line_st_boundary().min_s()
               << "] obstacle_traveled_s[" << obstacle_traveled_s << "]";

        // ignore the obstacle which is already on reference line and moving
        // along the direction of ADC
        if (obstacle_traveled_s < kepsilon &&
            obstacle->reference_line_st_boundary().min_t() <
                creep_config.ignore_max_st_min_t() &&
            obstacle->reference_line_st_boundary().min_s() >
                creep_config.ignore_min_st_min_s()) {
          continue;
        }
        all_far_away = false;
        break;
      }
    }

    creep_clear_counter_ = all_far_away ? creep_clear_counter_ + 1 : 0;
    if (creep_clear_counter_ >= 5) {
      creep_clear_counter_ = 0;  // reset
      creep_done = true;
    }
  }
  return creep_done;
}

}  // namespace planning
}  // namespace apollo
