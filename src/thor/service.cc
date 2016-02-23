#include <functional>
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
using namespace prime_server;

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/constants.h>
#include <valhalla/baldr/json.h>

#include "thor/service.h"

using namespace valhalla;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;


namespace {

const std::unordered_map<std::string, thor_worker_t::MATRIX_TYPE> MATRIX{
    {"one_to_many", thor_worker_t::ONE_TO_MANY},
    {"many_to_one", thor_worker_t::MANY_TO_ONE},
    {"many_to_many", thor_worker_t::MANY_TO_MANY}
  };

  std::size_t tdindex = 0;
  constexpr double kMilePerMeter = 0.000621371;
  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const headers_t::value_type JSON_MIME{"Content-type", "application/json;charset=utf-8"};
  const headers_t::value_type JS_MIME{"Content-type", "application/javascript;charset=utf-8"};

}

namespace valhalla {
  namespace thor {
    thor_worker_t::thor_worker_t(const boost::property_tree::ptree& config): mode(valhalla::sif::TravelMode::kPedestrian),
      config(config), reader(config.get_child("mjolnir.hierarchy")),
      long_request_route(config.get<float>("thor.logging.long_request_route")),
      long_request_manytomany(config.get<float>("thor.logging.long_request_manytomany")){
      // Register edge/node costing methods
      factory.Register("auto", sif::CreateAutoCost);
      factory.Register("auto_shorter", sif::CreateAutoShorterCost);
      factory.Register("bus", CreateBusCost);
      factory.Register("bicycle", sif::CreateBicycleCost);
      factory.Register("pedestrian", sif::CreatePedestrianCost);
      factory.Register("transit", sif::CreateTransitCost);
      factory.Register("truck", sif::CreateTruckCost);
    }

    worker_t::result_t thor_worker_t::work(const std::list<zmq::message_t>& job, void* request_info) {
      //get time for start of request
      auto s = std::chrono::system_clock::now();
      auto& info = *static_cast<http_request_t::info_t*>(request_info);
      LOG_INFO("Got Thor Request " + std::to_string(info.id));
      try{
        //get some info about what we need to do
        boost::property_tree::ptree request;
        std::string request_str(static_cast<const char*>(job.front().data()), job.front().size());
        std::stringstream stream(request_str);
        try {
          boost::property_tree::read_json(stream, request);
        }
        catch(const std::exception& e) {
          worker_t::result_t result{false};
          http_response_t response(500, "Internal Server Error", "Failed to parse intermediate request format", headers_t{CORS});
          response.from_info(info);
          result.messages.emplace_back(response.to_string());
          valhalla::midgard::logging::Log("500::" + std::string(e.what()), " [ANALYTICS] ");
          return result;
        }
        catch(...) {
          worker_t::result_t result{false};
          http_response_t response(500, "Internal Server Error", "Failed to parse intermediate request format", headers_t{CORS});
          response.from_info(info);
          result.messages.emplace_back(response.to_string());
          valhalla::midgard::logging::Log("500::non-std::exception", " [ANALYTICS] ");
          return result;
        }

        // Initialize request - get the PathALgorithm to use
        std::string costing = thor_worker_t::init_request(request);
        auto date_time_type = request.get_optional<int>("date_time.type");
        auto matrix = request.get_optional<std::string>("matrix_type");
        auto optimized = request.get_optional<bool>("optimized");
        if (matrix) {
          valhalla::midgard::logging::Log("matrix_type::" + *matrix, " [ANALYTICS] ");
          auto matrix_iter = MATRIX.find(*matrix);
          if (matrix_iter != MATRIX.cend()) {
            return thor_worker_t::matrix(matrix_iter->second, costing, request, info);
          }
          else { //this will never happen since loki formats the request for matrix
            throw std::runtime_error("Incorrect matrix_type provided:: " + *matrix + "  Accepted types are 'one_to_many', 'many_to_one' or 'many_to_many'.");
          }
        } else if (optimized)
          return thor_worker_t::optimized_path(correlated, costing, request_str);
        else
          return thor_worker_t::trip_path(costing, request_str, date_time_type);
      }

      catch(const std::exception& e) {
        worker_t::result_t result{false};
        http_response_t response(400, "Bad Request", e.what(), headers_t{CORS});
        response.from_info(info);
        result.messages.emplace_back(response.to_string());
        valhalla::midgard::logging::Log("400::" + std::string(e.what()), " [ANALYTICS] ");
        return result;
      }
    }

    /**
     * Update the origin edges for a through location.
     */
    void thor_worker_t::UpdateOrigin(baldr::PathLocation& origin, bool prior_is_node,
                      const baldr::GraphId& through_edge) {
      if (prior_is_node) {
        // TODO - remove the opposing through edge from list of edges unless
        // all outbound edges are entering noth_thru regions.
        // For now allow all edges
      } else {
        // Check if the edge is entering a not_thru region - if so do not
        // exclude the opposing edge
        const DirectedEdge* de = reader.GetGraphTile(through_edge)->directededge(through_edge);
        if (de->not_thru()) {
          return;
        }

        // Set the origin edge to the through_edge
        auto edges = origin.edges();
        for (auto e : edges) {
          if (e.id == through_edge) {
            origin.ClearEdges();
            origin.CorrelateEdge(e);
            break;
          }
        }
      }
    }

    void thor_worker_t::GetPath(PathAlgorithm* path_algorithm,
                 baldr::PathLocation& origin, baldr::PathLocation& destination,
                 std::vector<thor::PathInfo>& path_edges) {
      midgard::logging::Log("#_passes::1", " [ANALYTICS] ");
      // Find the path.
      path_edges = path_algorithm->GetBestPath(origin, destination, reader,
                                               mode_costing, mode);
      // If path is not found try again with relaxed limits (if allowed)
      if (path_edges.size() == 0) {
        valhalla::sif::cost_ptr_t cost = mode_costing[static_cast<uint32_t>(mode)];
        if (cost->AllowMultiPass()) {
          // 2nd pass
          path_algorithm->Clear();
          cost->RelaxHierarchyLimits(16.0f);
          midgard::logging::Log("#_passes::2", " [ANALYTICS] ");
          path_edges = path_algorithm->GetBestPath(origin, destination,
                                    reader, mode_costing, mode);
          // 3rd pass
          if (path_edges.size() == 0) {
            path_algorithm->Clear();
            cost->DisableHighwayTransitions();
            midgard::logging::Log("#_passes::3", " [ANALYTICS] ");
            path_edges = path_algorithm->GetBestPath(origin, destination,
                                     reader, mode_costing, mode);
          }
        }
      }
    }

    // Get the costing options. Get the base options from the config and the
    // options for the specified costing method. Merge in any request costing
    // options.
    valhalla::sif::cost_ptr_t thor_worker_t::get_costing(const boost::property_tree::ptree& request,
                                          const std::string& costing) {
      std::string method_options = "costing_options." + costing;
      auto config_costing = config.get_child_optional(method_options);
      if(!config_costing)
        throw std::runtime_error("No costing method found for '" + costing + "'");
      auto request_costing = request.get_child_optional(method_options);
      if(request_costing) {
        // If the request has any options for this costing type, merge the 2
        // costing options - override any config options that are in the request.
        // and add any request options not in the config.
        boost::property_tree::ptree overridden = *config_costing;
        for (const auto& r : *request_costing) {
          overridden.put_child(r.first, r.second);
        }
        return factory.Create(costing, overridden);
      }
      // No options to override so use the config options verbatim
      return factory.Create(costing, *config_costing);
    }

    std::string thor_worker_t::init_request(const boost::property_tree::ptree& request) {
      auto id = request.get_optional<std::string>("id");
      // Parse out units; if none specified, use kilometers
      double distance_scale = kKmPerMeter;
      auto units = request.get<std::string>("units", "km");
      if (units == "mi")
        distance_scale = kMilePerMeter;
      else {
        units = "km";
        distance_scale = kKmPerMeter;
      }
      //we require locations
      auto request_locations = request.get_child_optional("locations");
      if(!request_locations)
        throw std::runtime_error("Insufficiently specified required parameter 'locations'");
      for(const auto& location : *request_locations) {
        try{
          locations.push_back(baldr::Location::FromPtree(location.second));
        }
        catch (...) {
          throw std::runtime_error("Failed to parse location");
        }
      }
      if(locations.size() < 2)
        throw std::runtime_error("Insufficient number of locations provided");

      //type - 0: current, 1: depart, 2: arrive
      auto date_time_type = request.get_optional<int>("date_time.type");
      auto date_time_value = request.get_optional<std::string>("date_time.value");

      if (date_time_type == 0) //current.
        locations.front().date_time_ = "current";
      else if (date_time_type == 1) //depart at
        locations.front().date_time_ = date_time_value;
      else if (date_time_type == 2) //arrive)
        locations.back().date_time_ = date_time_value;

      //we require correlated locations
      size_t i = 0;
      do {
        auto path_location = request.get_child_optional("correlated_" + std::to_string(i));
        if(!path_location)
          break;
        try {
          correlated.emplace_back(PathLocation::FromPtree(locations, *path_location));
        }
        catch (...) {
          throw std::runtime_error("Failed to parse correlated location");
        }
      }while(++i);

      // Parse out the type of route - this provides the costing method to use
      auto costing = request.get_optional<std::string>("costing");
      if(!costing)
        throw std::runtime_error("No edge/node costing provided");

      // Set travel mode and construct costing
      if (*costing == "multimodal") {
        // For multi-modal we construct costing for all modes and set the
        // initial mode to pedestrian. (TODO - allow other initial modes)
        mode_costing[0] = get_costing(request, "auto");
        mode_costing[1] = get_costing(request, "pedestrian");
        mode_costing[2] = get_costing(request, "bicycle");
        mode_costing[3] = get_costing(request, "transit");
        mode = valhalla::sif::TravelMode::kPedestrian;
      } else {
        valhalla::sif::cost_ptr_t cost = get_costing(request, *costing);
        mode = cost->travelmode();
        mode_costing[static_cast<uint32_t>(mode)] = cost;
      }
      return *costing;
    }

    void thor_worker_t::cleanup() {
      astar.Clear();
      bidir_astar.Clear();
      multi_modal_astar.Clear();
      locations.clear();
      correlated.clear();
      if(reader.OverCommitted())
      reader.Clear();
    }

    void run_service(const boost::property_tree::ptree& config) {
      //gets requests from thor proxy
      auto upstream_endpoint = config.get<std::string>("thor.service.proxy") + "_out";
      //sends them on to odin
      auto downstream_endpoint = config.get<std::string>("odin.service.proxy") + "_in";
      //or returns just location information back to the server
      auto loopback_endpoint = config.get<std::string>("httpd.service.loopback");

      //listen for requests
      zmq::context_t context;
      thor_worker_t thor_worker(config);
      prime_server::worker_t worker(context, upstream_endpoint, downstream_endpoint, loopback_endpoint,
        std::bind(&thor_worker_t::work, std::ref(thor_worker), std::placeholders::_1, std::placeholders::_2),
        std::bind(&thor_worker_t::cleanup, std::ref(thor_worker)));
      worker.work();

      //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
    }
  }
}

