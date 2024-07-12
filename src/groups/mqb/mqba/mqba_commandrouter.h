// Copyright 2017-2023 Bloomberg Finance L.P.
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// mqbblp_messagegroupidmanager.h                                     -*-C++-*-
#ifndef INCLUDED_MQBA_COMMANDROUTER
#define INCLUDED_MQBA_COMMANDROUTER

//@PURPOSE: Provide a class responsible for routing admin commands to the
// subset of cluster nodes that should execute that command.
//
//@CLASSES:
//  mqbcmd::CommandRouter: Manages routing admin commands
//
//@DESCRIPTION:
//

// BSL
#include <bsl_iostream.h>
#include <bsl_string.h>
#include <bsl_vector.h>
#include <bslma_managedptr.h>
#include <bslmt_latch.h>
#include <bslstl_sharedptr.h>

// MQB
#include <mqbcmd_messages.h>

namespace BloombergLP {

// FORWARD DECLARATION
namespace bmqp_ctrlmsg {
class ControlMessage;
}
namespace mqbi {
class Cluster;
}
namespace mqbnet {
class ClusterNode;
}
namespace mqbnet {
template <class REQUEST, class RESPONSE, class TARGET>
class MultiRequestManagerRequestContext;
}

namespace mqba {

class RouteCommandManager {
  public:
    typedef bsl::shared_ptr<
        mqbnet::MultiRequestManagerRequestContext<bmqp_ctrlmsg::ControlMessage,
                                                  bmqp_ctrlmsg::ControlMessage,
                                                  mqbnet::ClusterNode*> >
                                              MultiRequestContextSp;
    typedef bsl::vector<mqbnet::ClusterNode*> NodesVector;

  private:
    struct RouteMembers {
        NodesVector nodes;
        bool        self;
    };

    class RoutingMode {
      public:
        RoutingMode();
        virtual ~RoutingMode() = 0;

        virtual RouteMembers getRouteMembers(mqbi::Cluster* cluster) = 0;
    };
    class AllPartitionPrimariesRoutingMode : public RoutingMode {
      public:
        AllPartitionPrimariesRoutingMode();

        RouteMembers
        getRouteMembers(mqbi::Cluster* cluster) BSLS_KEYWORD_OVERRIDE;
    };
    class SinglePartitionPrimaryRoutingMode : public RoutingMode {
      private:
        int d_partitionId;

      public:
        SinglePartitionPrimaryRoutingMode(int partitionId);

        RouteMembers
        getRouteMembers(mqbi::Cluster* cluster) BSLS_KEYWORD_OVERRIDE;
    };
    class ClusterRoutingMode : public RoutingMode {
      public:
        ClusterRoutingMode();

        RouteMembers
        getRouteMembers(mqbi::Cluster* cluster) BSLS_KEYWORD_OVERRIDE;
    };

  public:
    typedef bslma::ManagedPtr<RoutingMode> RoutingModeMp;

  private:
    const bsl::string&           d_commandString;
    const mqbcmd::CommandChoice& d_command;

    mqbcmd::RouteResponseList d_responses;

    RoutingModeMp d_routingMode;

    bslmt::Latch d_latch;

  public:
    /// Sets up a command router with the given command string and parsed
    /// command object. This will
    RouteCommandManager(const bsl::string&           commandString,
                        const mqbcmd::CommandChoice& command);

    /// Returns true if this command router is necessary to route the command
    /// that it was set up with. If the command does not require routing, then
    /// this function returns false.
    bool isRoutingNeeded() const;

    /// Performs any routing on the command and returns true if the caller
    /// should also execute the command.
    bool route(mqbi::Cluster* relevantCluster);

    /// Waits on a latch that triggers when the responses have been received.
    void waitForResponses();

    /// Returns a reference to the collected responses from routing.
    // ResponseMessages& responses();
    mqbcmd::RouteResponseList& responses();

    /// Returns a pointer to the relevant cluster for this
    /// command. The pointer can be guaranteed to be non-null.
    // mqbi::Cluster* cluster() const;

  private:
    RoutingModeMp getCommandRoutingMode();

    void countDownLatch();

    void onRouteCommandResponse(const MultiRequestContextSp& requestContext);

    void routeCommand(const NodesVector& nodes);
};

inline bool RouteCommandManager::isRoutingNeeded() const
{
    return d_routingMode.get() != nullptr;
}

inline mqbcmd::RouteResponseList& RouteCommandManager::responses()
{
    return d_responses;
}

inline void RouteCommandManager::waitForResponses()
{
    d_latch.wait();
}

inline void RouteCommandManager::countDownLatch()
{
    d_latch.countDown(1);
}

}  // close package namespace
}  // close enterprise namespace

#endif
