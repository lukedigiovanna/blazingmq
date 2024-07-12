// Copyright 2014-2023 Bloomberg Finance L.P.
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

// mqba_commandrouter.cpp
#include <mqba_commandrouter.h>

// MQB
#include <mqbcmd_parseutil.h>
#include <mqbi_cluster.h>
#include <mqbnet_cluster.h>
#include <mqbnet_multirequestmanager.h>

// BDE
#include <ball_log.h>
#include <bsl_iostream.h>

namespace BloombergLP {
namespace mqba {

namespace {
const char k_LOG_CATEGORY[] = "MQBA.COMMANDROUTER";
}  // close unnamed namespace

RouteCommandManager::RouteCommandManager(const bsl::string& commandString,
                                         const mqbcmd::CommandChoice& command)
: d_commandString(commandString)
, d_command(command)
, d_routingMode(getCommandRoutingMode())
, d_latch(1)
{
    // Effectively negate the latch if we won't need to wait for responses.
    if (!isRoutingNeeded()) {
        countDownLatch();
    }
}

RouteCommandManager::RoutingMode::RoutingMode()
{
}

RouteCommandManager::RoutingMode::~RoutingMode()
{
}

RouteCommandManager::AllPartitionPrimariesRoutingMode::
    AllPartitionPrimariesRoutingMode()
{
}

RouteCommandManager::RouteMembers
RouteCommandManager::AllPartitionPrimariesRoutingMode::getRouteMembers(
    mqbi::Cluster* cluster)
{
    NodesVector primaryNodes;
    bool        isSelfPrimary;

    cluster->dispatcher()->execute(
        bdlf::BindUtil::bind(&mqbi::Cluster::getPrimaryNodes,
                             cluster,
                             &primaryNodes,
                             &isSelfPrimary),
        cluster);

    cluster->dispatcher()->synchronize(cluster);

    return {primaryNodes, isSelfPrimary};
}

RouteCommandManager::SinglePartitionPrimaryRoutingMode::
    SinglePartitionPrimaryRoutingMode(int partitionId)
: d_partitionId(partitionId)
{
}

RouteCommandManager::RouteMembers
RouteCommandManager::SinglePartitionPrimaryRoutingMode::getRouteMembers(
    mqbi::Cluster* cluster)
{
    mqbnet::ClusterNode* node          = nullptr;
    bool                 isSelfPrimary = false;

    cluster->dispatcher()->execute(
        bdlf::BindUtil::bind(&mqbi::Cluster::getPartitionPrimaryNode,
                             cluster,
                             &node,
                             &isSelfPrimary,
                             d_partitionId),
        cluster);

    cluster->dispatcher()->synchronize(cluster);

    NodesVector nodes;
    if (node) {
        // Put node into vector to be acceptable for "routeCommand"
        nodes.push_back(node);
    }

    return {nodes, isSelfPrimary};
}

RouteCommandManager::ClusterRoutingMode::ClusterRoutingMode()
{
}

RouteCommandManager::RouteMembers
RouteCommandManager::ClusterRoutingMode::getRouteMembers(
    mqbi::Cluster* cluster)
{
    typedef mqbnet::Cluster::NodesList NodesList;
    // collect all nodes in cluster
    const NodesList& allNodes = cluster->netCluster().nodes();

    NodesVector nodes;

    for (NodesList::const_iterator nit = allNodes.begin();
         nit != allNodes.end();
         nit++) {
        if (cluster->netCluster().selfNode() != *nit) {
            nodes.push_back(*nit);
        }
    }

    return {
        nodes,
        true  // Cluster routing always requires original node to exec.
    };
}

bool RouteCommandManager::route(mqbi::Cluster* relevantCluster)
{
    // PRECONDITIONS
    BSLS_ASSERT_SAFE(d_routingMode);
    BSLS_ASSERT_SAFE(relevantCluster);

    typedef mqbnet::MultiRequestManager<bmqp_ctrlmsg::ControlMessage,
                                        bmqp_ctrlmsg::ControlMessage,
                                        mqbnet::ClusterNode*>::RequestContextSp
        RequestContextSp;

    RouteMembers routeMembers = d_routingMode->getRouteMembers(
        relevantCluster);

    if (routeMembers.nodes.size() == 0) {
        countDownLatch();
        return routeMembers.self;
    }

    RequestContextSp contextSp =
        relevantCluster->multiRequestManager().createRequestContext();

    bmqp_ctrlmsg::AdminCommand& adminCommand =
        contextSp->request().choice().makeAdminCommand();

    adminCommand.command()  = d_commandString;
    adminCommand.rerouted() = true;

    contextSp->setDestinationNodes(routeMembers.nodes);

    mwcu::MemOutStream os;
    os << "Routing command to the following nodes [";
    for (NodesVector::const_iterator nit = routeMembers.nodes.begin();
         nit != routeMembers.nodes.end();
         nit++) {
        os << (*nit)->hostName();
        if (nit + 1 != routeMembers.nodes.end()) {
            os << ", ";
        }
    }
    os << "]";
    BALL_LOG_SET_CATEGORY(k_LOG_CATEGORY);
    BALL_LOG_INFO << os.str();

    contextSp->setResponseCb(
        bdlf::BindUtil::bind(&RouteCommandManager::onRouteCommandResponse,
                             this,
                             bdlf::PlaceHolders::_1));

    relevantCluster->multiRequestManager().sendRequest(contextSp,
                                                       bsls::TimeInterval(3));

    return routeMembers.self;
}

void RouteCommandManager::onRouteCommandResponse(
    const MultiRequestContextSp& requestContext)
{
    typedef bsl::pair<mqbnet::ClusterNode*, bmqp_ctrlmsg::ControlMessage>
                                  NodePair;
    typedef bsl::vector<NodePair> NodePairsVector;

    NodePairsVector responsePairs = requestContext->response();

    for (NodePairsVector::const_iterator pairIt = responsePairs.begin();
         pairIt != responsePairs.end();
         pairIt++) {
        NodePair pair = *pairIt;

        bmqp_ctrlmsg::ControlMessage& message = pair.second;

        mqbcmd::RouteResponse routeResponse;
        routeResponse.sourceNodeDescription() = pair.first->hostName();
        if (message.choice().isAdminCommandResponseValue()) {
            const bsl::string& output =
                message.choice().adminCommandResponse().text();
            routeResponse.response() = output;
        }
        else {
            // Something went wrong, possibly timed out
            routeResponse.response() =
                "Error ocurred sending command to node " +
                pair.first->hostName();
        }
        d_responses.responses().push_back(routeResponse);
    }

    countDownLatch();
}

RouteCommandManager::RoutingModeMp RouteCommandManager::getCommandRoutingMode()
{
    RouteCommandManager::RoutingMode* routingMode = nullptr;
    if (d_command.isDomainsValue()) {
        const mqbcmd::DomainsCommand& domains = d_command.domains();
        if (domains.isDomainValue()) {
            const mqbcmd::DomainCommand& domain = domains.domain().command();
            if (domain.isPurgeValue()) {
                routingMode =
                    new AllPartitionPrimariesRoutingMode();  // DOMAINS DOMAIN
                                                             // <name> PURGE
            }
            else if (domain.isQueueValue()) {
                if (domain.queue().command().isPurgeAppIdValue()) {
                    routingMode =
                        new AllPartitionPrimariesRoutingMode();  // DOMAINS
                                                                 // DOMAIN
                                                                 // <name>
                                                                 // QUEUE
                                                                 // <name>
                                                                 // PURGE
                }
            }
        }
        else if (domains.isReconfigureValue()) {
            routingMode =
                new ClusterRoutingMode();  // DOMAINS RECONFIGURE <domain>
        }
    }
    else if (d_command.isClustersValue()) {
        const mqbcmd::ClustersCommand& clusters = d_command.clusters();
        if (clusters.isClusterValue()) {
            const mqbcmd::ClusterCommand& cluster =
                clusters.cluster().command();
            if (cluster.isForceGcQueuesValue()) {
                routingMode =
                    new AllPartitionPrimariesRoutingMode();  // CLUSTERS
                                                             // CLUSTER <name>
                                                             // FORCE_GC_QUEUES
            }
            else if (cluster.isStorageValue()) {
                const mqbcmd::StorageCommand& storage = cluster.storage();
                if (storage.isPartitionValue()) {
                    if (storage.partition().command().isEnableValue() ||
                        storage.partition().command().isDisableValue()) {
                        int partitionId = storage.partition().partitionId();
                        routingMode = new SinglePartitionPrimaryRoutingMode(
                            partitionId);  // CLUSTERS CLUSTER <name> STORAGE
                                           // PARTITION <partitionId>
                                           // [ENABLE|DISABLE]
                    }
                    // SUMMARY doesn't need to route to primary
                }
                if (storage.isReplicationValue()) {
                    const mqbcmd::ReplicationCommand& replication =
                        storage.replication();
                    if (replication.isSetTunableValue()) {
                        const mqbcmd::SetTunable& tunable =
                            replication.setTunable();
                        if (tunable.choice().isAllValue()) {
                            routingMode =
                                new ClusterRoutingMode();  // CLUSTERS CLUSTER
                                                           // <name> STORAGE
                                                           // REPLICATION
                                                           // SET_ALL
                        }
                    }
                    else if (replication.isGetTunableValue()) {
                        const mqbcmd::GetTunable& tunable =
                            replication.getTunable();
                        if (tunable.choice().isAllValue()) {
                            routingMode =
                                new ClusterRoutingMode();  // CLUSTERS CLUSTER
                                                           // <name> STORAGE
                                                           // REPLICATION
                                                           // GET_ALL
                        }
                    }
                }
            }
            else if (cluster.isStateValue()) {
                const mqbcmd::ClusterStateCommand state = cluster.state();
                if (state.isElectorValue()) {
                    const mqbcmd::ElectorCommand elector = state.elector();
                    if (elector.isSetTunableValue()) {
                        const mqbcmd::SetTunable& tunable =
                            elector.setTunable();
                        if (tunable.choice().isAllValue()) {
                            routingMode =
                                new ClusterRoutingMode();  // CLUSTERS CLUSTER
                                                           // <name> STATE
                                                           // ELECTOR SET_ALL
                        }
                    }
                    else if (elector.isGetTunableValue()) {
                        const mqbcmd::GetTunable& tunable =
                            elector.getTunable();
                        if (tunable.choice().isAllValue()) {
                            routingMode =
                                new ClusterRoutingMode();  // CLUSTERS CLUSTER
                                                           // <name> STATE
                                                           // ELECTOR GET_ALL
                        }
                    }
                }
            }
        }
    }

    bslma::ManagedPtr<RoutingMode> ret(routingMode);

    return ret;
}

}  // close package namespace
}  // close enterprise namespace
