#include "L1Trigger/TrackerDTC/interface/DTC.h"
#include "L1Trigger/TrackerDTC/interface/Settings.h"
#include "L1Trigger/TrackerDTC/interface/Module.h"

#include <vector>
#include <deque>
#include <iterator>
#include <algorithm>
#include <utility>
#include <numeric>

using namespace std;
using namespace edm;

namespace trackerDTC {

  DTC::DTC(Settings* settings, int dtcId, const std::vector<std::vector<TTStubRef>>& modules)
      : settings_(settings),
        region_(dtcId / settings_->numDTCsPerRegion()),
        board_(dtcId % settings_->numDTCsPerRegion()),
        modules_(settings_->modules(dtcId)),
        input_(settings_->numRoutingBlocks(), Stubss(settings_->numModulesPerRoutingBlock())),
        lost_(settings_->numOverlappingRegions()) {
    // count number of stubs on this dtc
    auto acc = [](int& sum, const vector<TTStubRef>& module) { return sum += module.size(); };
    const int nStubs = accumulate(modules.begin(), modules.end(), 0, acc);
    stubs_.reserve(nStubs);
    // convert and assign Stubs to DTC routing block channel
    for (int modId = 0; modId < settings_->numModulesPerDTC(); modId++) {
      const vector<TTStubRef>& ttStubRefs = modules[modId];
      if (ttStubRefs.empty())
        continue;
      // Module which produced this ttStubRefs
      Module* module = modules_.at(modId);
      // DTC routing block id [0-1]
      const int blockId = modId / settings_->numModulesPerRoutingBlock();
      // DTC routing blockc  channel id [0-35]
      const int channelId = modId % settings_->numModulesPerRoutingBlock();
      // convert TTStubs and fill input channel
      Stubs& stubs = input_[blockId][channelId];
      for (const TTStubRef& ttStubRef : ttStubRefs) {
        stubs_.emplace_back(settings_, module, ttStubRef);
        Stub& stub = stubs_.back();
        if (stub.valid())
          // passed pt and eta cut
          stubs.push_back(&stub);
      }
      // sort stubs by bend
      sort(stubs.begin(), stubs.end(), [](Stub* lhs, Stub* rhs) { return abs(lhs->bend()) < abs(rhs->bend()); });
      // truncate stubs if desired
      if (!settings_->enableTruncation() || (int)stubs.size() <= settings_->maxFramesChannelInput())
        continue;
      // begin of truncated stubs
      const auto limit = next(stubs.begin(), settings_->maxFramesChannelInput());
      // copy truncated stubs into lost output channel
      for (int region = 0; region < settings_->numOverlappingRegions(); region++)
        copy_if(
            limit, stubs.end(), back_inserter(lost_[region]), [region](Stub* stub) { return stub->inRegion(region); });
      // remove truncated stubs form input channel
      stubs.erase(limit, stubs.end());
    }
  }

  // board level routing in two steps and products filling
  void DTC::produce(TTDTC& productAccepted, TTDTC& productLost) {
    // router step 1: merges stubs of all modules connected to one routing block into one stream
    Stubs lost;
    Stubss blockStubs(settings_->numRoutingBlocks());
    for (int routingBlock = 0; routingBlock < settings_->numRoutingBlocks(); routingBlock++)
      merge(input_[routingBlock], blockStubs[routingBlock], lost);
    // copy lost stubs during merge into lost output channel
    for (int region = 0; region < settings_->numOverlappingRegions(); region++)
      copy_if(lost.begin(), lost.end(), back_inserter(lost_[region]), [region](Stub* stub) {
        return stub->inRegion(region);
      });
    // router step 2: merges stubs of all routing blocks and splits stubs into one stream per overlapping region
    Stubss regionStubs(settings_->numOverlappingRegions());
    split(blockStubs, regionStubs);
    // fill products
    produce(regionStubs, productAccepted);
    produce(lost_, productLost);
  }

  // router step 1: merges stubs of all modules connected to one routing block into one stream
  void DTC::merge(Stubss& inputs, Stubs& output, Stubs& lost) {
    // for each input one fifo
    Stubss stacks(inputs.size());
    // clock accurate firmware emulation, each while trip describes one clock tick
    while (!all_of(inputs.begin(), inputs.end(), [](const Stubs& channel) { return channel.empty(); }) or
           !all_of(stacks.begin(), stacks.end(), [](const Stubs& channel) { return channel.empty(); })) {
      // fill fifos
      for (int iInput = 0; iInput < (int)inputs.size(); iInput++) {
        Stubs& input = inputs[iInput];
        Stubs& stack = stacks[iInput];
        if (input.empty())
          continue;
        Stub* stub = pop_front(input);
        if (stub) {
          if (settings_->enableTruncation() && (int)stack.size() == settings_->sizeStack() - 1)
            // kill current first stub when fifo overflows
            lost.push_back(pop_front(stack));
          stack.push_back(stub);
        }
      }
      // route stub from a fifo to output if possible
      bool nothingToRoute(true);
      for (int iInput = inputs.size() - 1; iInput >= 0; iInput--) {
        Stubs& stack = stacks[iInput];
        if (stack.empty())
          continue;
        nothingToRoute = false;
        output.push_back(pop_front(stack));
        // only one stub can be routed to output per clock tick
        break;
      }
      // each clock tick output will grow by one, if no stub is available then by a gap
      if (nothingToRoute)
        output.push_back(nullptr);
    }
    // truncate if desired
    if (settings_->enableTruncation() && (int)output.size() > settings_->maxFramesChannelOutput()) {
      const auto limit = next(output.begin(), settings_->maxFramesChannelOutput());
      copy_if(limit, output.end(), back_inserter(lost), [](Stub* stub) { return stub; });
      output.erase(limit, output.end());
    }
    // remove all gaps between end and last stub
    for (auto it = output.end(); it != output.begin();)
      it = (*--it) ? output.begin() : output.erase(it);
  }

  // router step 2: merges stubs of all routing blocks and splits stubs into one stream per overlapping region
  void DTC::split(Stubss& inputs, Stubss& outputs) {
    int region(0);
    auto regionMask = [&region](Stub* stub) { return stub && stub->inRegion(region) ? stub : nullptr; };
    for (Stubs& output : outputs) {
      // copy of masked inputs for each output
      Stubss streams(inputs.size());
      int i(0);
      for (Stubs& input : inputs)
        transform(input.begin(), input.end(), back_inserter(streams[i++]), regionMask);
      merge(streams, output, lost_[region++]);
    }
  }

  // conversion from Stubss to TTDTC
  void DTC::produce(const Stubss& stubss, TTDTC& product) {
    int channel(0);
    auto toFrame = [&channel](Stub* stub) {
      return stub ? make_pair(stub->ttStubRef(), stub->frame(channel)) : TTDTC::Frame();
    };
    for (const Stubs& stubs : stubss) {
      TTDTC::Stream stream;
      stream.reserve(stubs.size());
      transform(stubs.begin(), stubs.end(), back_inserter(stream), toFrame);
      product.setStream(region_, board_, channel++, stream);
    }
  }

  // pop_front function which additionally returns copy of deleted front
  Stub* DTC::pop_front(Stubs& deque) {
    Stub* stub = deque.front();
    deque.pop_front();
    return stub;
  }

}  // namespace trackerDTC