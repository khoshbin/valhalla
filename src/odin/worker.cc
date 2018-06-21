#include <fstream>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "baldr/json.h"
#include "midgard/logging.h"

#include "odin/directionsbuilder.h"
#include "odin/util.h"
#include "odin/worker.h"
#include "tyr/serializers.h"

#include <valhalla/proto/trippath.pb.h>
#include <valhalla/proto/directions.pb.h>

using namespace valhalla;

namespace valhalla {
namespace odin {

OdinWorker::OdinWorker(const boost::property_tree::ptree& config) {
}

OdinWorker::~OdinWorker() {
}

void OdinWorker::cleanup() {
}

std::list<TripDirections> OdinWorker::narrate(const valhalla_request_t& request,
                                                 std::list<TripPath>& legs) const {
  // get some annotated directions
  std::list<TripDirections> narrated;
  try {
    for (auto& leg : legs) {
      narrated.emplace_back(odin::DirectionsBuilder().Build(request.options, leg));
      LOG_INFO("maneuver_count::" + std::to_string(narrated.back().maneuver_size()));
    }
  } catch (...) { throw valhalla_exception_t{202}; }
  return narrated;
}

proto::Directions OdinWorker::narrateProto(const valhalla_request_t& request,
                                                 std::list<TripPath>& legs) const {
  // get some annotated directions
  return odin::DirectionsBuilder().BuildProto(request.options, legs);
}

#ifdef HAVE_HTTP
worker_t::result_t OdinWorker::work(const std::list<zmq::message_t>& job,
                                       void* request_info,
                                       const std::function<void()>& interrupt_function) {
  auto& info = *static_cast<http_request_info_t*>(request_info);
  LOG_INFO("Got Odin Request " + std::to_string(info.id));
  valhalla_request_t request;
  try {
    // crack open the original request
    std::string request_str(static_cast<const char*>(job.front().data()), job.front().size());
    std::string serialized_options(static_cast<const char*>((++job.cbegin())->data()),
                                   (++job.cbegin())->size());
    request.parse(request_str, serialized_options);

    // Set the interrupt function
    service_worker_t::set_interrupt(interrupt_function);

    // parse each leg
    std::list<TripPath> legs;
    for (auto leg = ++(++job.cbegin()); leg != job.cend(); ++leg) {
      // crack open the path
      legs.emplace_back();
      try {
        legs.back().ParseFromArray(leg->data(), static_cast<int>(leg->size()));
      } catch (...) { return jsonify_error({201}, info, request); }
    }

    // narrate them and serialize them along
    if (request.options.format() == DirectionsOptions::proto) {

      proto::Directions narrated_proto = narrateProto(request, legs);

      // encode to binary
      std::string narrated_proto_string;
      narrated_proto.SerializeToString(&narrated_proto_string);

      // // decode again
      // proto::Directions decoded;
      // decoded.ParseFromString(narrated_proto_string);

      // // print for debugging purposes
      // std::cout << narrated_proto.DebugString() << std::endl;

      // // save to file
      // std::ofstream debugfile;
      // debugfile.open ("data/debug.pbf");
      // debugfile << narrated_proto_string;
      // debugfile.close();

      return to_response_proto(narrated_proto_string, info, request);
    }
    else {
      auto narrated = narrate(request, legs);
      auto response = tyr::serializeDirections(request, legs, narrated);
      auto* to_response =
          request.options.format() == DirectionsOptions::gpx ? to_response_xml : to_response_json;
      return to_response(response, info, request);
    }
  } catch (const std::exception& e) {
    return jsonify_error({299, std::string(e.what())}, info, request);
  }
}

void run_service(const boost::property_tree::ptree& config) {
  // gets requests from odin proxy
  auto upstream_endpoint = config.get<std::string>("odin.service.proxy") + "_out";
  // or returns just location information back to the server
  auto loopback_endpoint = config.get<std::string>("httpd.service.loopback");
  auto interrupt_endpoint = config.get<std::string>("httpd.service.interrupt");

  // listen for requests
  zmq::context_t context;
  prime_server::worker_t worker(context, upstream_endpoint, "ipc:///dev/null", loopback_endpoint,
                                interrupt_endpoint,
                                std::bind(&OdinWorker::work, OdinWorker(config),
                                          std::placeholders::_1, std::placeholders::_2,
                                          std::placeholders::_3));
  worker.work();

  // TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
}

#endif
} // namespace odin
} // namespace valhalla
