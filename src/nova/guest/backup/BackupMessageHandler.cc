#include "pch.hpp"
#include "nova/guest/backup/BackupMessageHandler.h"
#include "nova/guest/GuestException.h"
#include <boost/foreach.hpp>
#include "nova/Log.h"
#include <sstream>
#include <string>

using nova::JsonData;
using nova::JsonDataPtr;
using nova::Log;
using nova::guest::GuestException;
using boost::optional;
using std::string;
using std::stringstream;
using namespace boost;

namespace nova { namespace guest { namespace backup {

BackupMessageHandler::BackupMessageHandler(BackupManager & backup_manager)
: backup_manager(backup_manager) {
}


JsonDataPtr BackupMessageHandler::handle_message(const GuestInput & input) {
    NOVA_LOG_DEBUG("entering the handle_message method now ");
    if (input.method_name == "create_backup") {
        NOVA_LOG_DEBUG("handling the create_backup method");
        const auto id = input.args->get_string("backup_id");
        const auto swift_url = input.args->get_string("swift_url");
        if (!input.tenant) {
            NOVA_LOG_ERROR("Tenant was not specified by this RPC call! "
                           "Aborting...");
            throw GuestException(GuestException::MALFORMED_INPUT);
        }
        if (!input.token) {
            NOVA_LOG_ERROR("Token was not specified by this RPC call! "
                           "Aborting...");
            throw GuestException(GuestException::MALFORMED_INPUT);
        }
        const auto tenant = input.tenant.get();
        const auto token = input.token.get();
        backup_manager.run_backup(swift_url, tenant, token, id);
        return JsonData::from_null();
    } else {
        return JsonDataPtr();
    }
}



} } } // end namespace nova::guest::backup
