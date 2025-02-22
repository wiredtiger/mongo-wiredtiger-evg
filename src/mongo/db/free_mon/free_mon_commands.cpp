/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/free_mon/free_mon_commands_gen.h"
#include "mongo/db/free_mon/free_mon_controller.h"
#include "mongo/db/free_mon/free_mon_storage.h"

namespace mongo {

namespace {

const auto kRegisterSyncTimeout = Milliseconds{100};

/**
 * Indicates the current status of Free Monitoring.
 */
class GetFreeMonitoringStatusCommand : public BasicCommand {
public:
    GetFreeMonitoringStatusCommand() : BasicCommand("getFreeMonitoringStatus") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    std::string help() const final {
        return "Indicates free monitoring status";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::checkFreeMonitoringStatus)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        // Command has no members, invoke the parser to confirm that.
        IDLParserErrorContext ctx("getFreeMonitoringStatus");
        GetFreeMonitoringStatus::parse(ctx, cmdObj);

        FreeMonStorage::getStatus(opCtx, &result);
        return true;
    }
} getFreeMonitoringStatusCommand;

/**
 * Enables or disables Free Monitoring service.
 */
class SetFreeMonitoringCommand : public BasicCommand {
public:
    SetFreeMonitoringCommand() : BasicCommand("setFreeMonitoring") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    std::string help() const final {
        return "enable or disable Free Monitoring";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::setFreeMonitoring)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        IDLParserErrorContext ctx("setFreeMonitoring");
        auto cmd = SetFreeMonitoring::parse(ctx, cmdObj);

        auto* controller = FreeMonController::get(opCtx->getServiceContext());
        boost::optional<Status> optStatus = boost::none;
        if (cmd.getAction() == SetFreeMonActionEnum::enable) {
            optStatus = controller->registerServerCommand(kRegisterSyncTimeout);
        } else {
            optStatus = controller->unregisterServerCommand();
        }

        if (optStatus) {
            // Completed within timeout.
            return CommandHelpers::appendCommandStatus(result, *optStatus);
        } else {
            // Pending operation.
            return CommandHelpers::appendCommandStatus(result, Status::OK());
        }
    }

} setFreeMonitoringCmd;

}  // namespace
}  // namespace mongo
