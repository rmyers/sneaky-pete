#include "pch.hpp"
#include "nova/rpc/amqp.h"
#include "nova/guest/apt.h"
#include "nova/ConfigFile.h"
#include "nova/utils/Curl.h"
#include "nova/flags.h"
#include <boost/assign/list_of.hpp>
#include "nova/LogFlags.h"
#include <boost/format.hpp>
#include "nova/guest/guest.h"
#include "nova/guest/diagnostics.h"
#include "nova/guest/backup/BackupManager.h"
#include "nova/guest/backup/BackupMessageHandler.h"
#include "nova/guest/monitoring/monitoring.h"
#include "nova/db/mysql.h"
#include <boost/foreach.hpp>
#include "nova/guest/GuestException.h"
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <memory>
#include "nova/db/mysql.h"
#include "nova/guest/mysql/MySqlMessageHandler.h"
#include <boost/optional.hpp>
#include "nova/rpc/receiver.h"
#include <json/json.h>
#include "nova/Log.h"
#include <sstream>
#include <boost/thread.hpp>
#include "nova/utils/threads.h"
#include <boost/tuple/tuple.hpp>
#include "nova/guest/utils.h"
#include <vector>

#include "nova/guest/agent.h"


using namespace boost::assign;
using nova::guest::apt::AptGuest;
using nova::guest::apt::AptGuestPtr;
using nova::guest::apt::AptMessageHandler;
using std::auto_ptr;
using nova::guest::backup::BackupManager;
using nova::guest::backup::BackupMessageHandler;
using nova::guest::backup::BackupRestoreManager;
using nova::guest::backup::BackupRestoreManagerPtr;
using nova::process::CommandList;
using nova::utils::CurlScope;
using boost::format;
using boost::optional;
using namespace nova;
using namespace nova::flags;
using namespace nova::guest;
using namespace nova::guest::diagnostics;
using namespace nova::guest::monitoring;
using namespace nova::db::mysql;
using namespace nova::guest::mysql;
using nova::utils::ThreadBasedJobRunner;
using namespace nova::rpc;
using std::string;
using nova::utils::Thread;
using std::vector;

// Begin anonymous namespace.
namespace {

class PeriodicTasks
{
public:
    PeriodicTasks(MonitoringManagerPtr monitoring_manager,
                  MySqlAppStatusPtr mysql_app_status)
    : monitoring_manager(monitoring_manager),
      mysql_app_status(mysql_app_status)
    {
    }

    void operator() ()
    {
        mysql_app_status->update();
        monitoring_manager->ensure_running();
    }

private:
    MonitoringManagerPtr monitoring_manager;
    MySqlAppStatusPtr mysql_app_status;
};

typedef boost::shared_ptr<PeriodicTasks> PeriodicTasksPtr;


struct Func {

    // Initialize curl.
    nova::utils::CurlScope scope;

    // Initialize MySQL libraries. This should be done before spawning
    // threads.
    nova::db::mysql::MySqlApiScope mysql_api_scope;

    static bool is_mysql_installed(std::list<std::string> package_list,
                                   AptGuestPtr & apt_worker) {
        BOOST_FOREACH(const auto & package_name, package_list) {
            if (apt_worker->version(package_name.c_str())) {
                return true;
            }
        }
        return false;
    }

    boost::tuple<std::vector<MessageHandlerPtr>, PeriodicTasksPtr>
        operator() (const FlagValues & flags,
                    ResilientSenderPtr sender,
                    ThreadBasedJobRunner & job_runner)
    {
        /* Create JSON message handlers. */
        vector<MessageHandlerPtr> handlers;

        /* Create Apt Guest */
        AptGuestPtr apt_worker(new AptGuest(
            flags.apt_use_sudo(),
            flags.apt_self_package_name(),
            flags.apt_self_update_time_out()));
        MessageHandlerPtr handler_apt(new AptMessageHandler(apt_worker));
        handlers.push_back(handler_apt);

        const auto package_list = flags.possible_packages_for_mysql();

        /* Create MySQL updater. */
        MySqlAppStatusPtr mysql_status_updater(new MySqlAppStatus(
            sender, is_mysql_installed(package_list, apt_worker)));

        /* Create MySQL Guest. */
        MessageHandlerPtr handler_mysql(new MySqlMessageHandler());
        handlers.push_back(handler_mysql);

        MonitoringManagerPtr monitoring_manager(new MonitoringManager(
            flags.guest_id(),
            flags.monitoring_agent_package_name(),
            flags.monitoring_agent_config_file(),
            flags.monitoring_agent_install_timeout()));
        MessageHandlerPtr handler_monitoring_app(new MonitoringMessageHandler(
            apt_worker, monitoring_manager));
        handlers.push_back(handler_monitoring_app);

        BackupRestoreManagerPtr backup_restore_manager(new BackupRestoreManager(
            flags.backup_restore_process_commands(),
            flags.backup_restore_delete_file_pattern(),
            flags.backup_restore_restore_directory(),
            flags.backup_restore_save_file_pattern(),
            flags.backup_restore_zlib_buffer_size()
        ));
        MySqlAppPtr mysqlApp(new MySqlApp(mysql_status_updater,
                                          backup_restore_manager,
                                          flags.mysql_state_change_wait_time(),
                                          flags.skip_install_for_prepare()));

        /** Sneaky Pete formats and mounts volumes based on the bool flag
          *'volume_format_and_mount'.
          * If disabled a volumeManager null pointer is passed to the mysql
          * message handler. */
        VolumeManagerPtr volumeManager;
        if (flags.volume_format_and_mount()) {
          /** Create Volume Manager.
            * Reset null pointer to reference VolumeManager */
          volumeManager.reset(new VolumeManager(
            flags.volume_check_device_num_retries(),
            flags.volume_file_system_type(),
            flags.volume_format_options(),
            flags.volume_format_timeout(),
            "/var/lib/mysql",
            flags.volume_mount_options()
          ));
        }

        /** TODO (joe.cruz) There has to be a better way of enabling sneaky pete to
          * format/mount a volume and to create the volume manager based on that.
          * I did this because currently flags can only be retrived from
          * receiver_daemon so the volumeManager has to be created here. */
        MessageHandlerPtr handler_mysql_app(
            new MySqlAppMessageHandler(mysqlApp,
                                       apt_worker,
                                       monitoring_manager,
                                       volumeManager));
        handlers.push_back(handler_mysql_app);

        /* Create the Interrogator for the guest. */
        Interrogator interrogator;
        MessageHandlerPtr handler_interrogator(
            new InterrogatorMessageHandler(interrogator));
        handlers.push_back(handler_interrogator);

        /* Backup task */
        BackupManager backup(
                      sender,
                      job_runner,
                      flags.backup_process_commands(),
                      flags.backup_segment_max_size(),
                      flags.checksum_wait_time(),
                      flags.backup_swift_container(),
                      flags.backup_timeout(),
                      flags.backup_zlib_buffer_size());
        MessageHandlerPtr handler_backup(new BackupMessageHandler(backup));
        handlers.push_back(handler_backup);

        if (flags.register_dangerous_functions()) {
            NOVA_LOG_INFO("WARNING! Dangerous functions will be callable!");
            MessageHandlerPtr handler_diagnostics(
                new DiagnosticsMessageHandler(true));
            handlers.push_back(handler_diagnostics);
        }

        PeriodicTasksPtr updater(new PeriodicTasks(
          monitoring_manager, mysql_status_updater
          ));

        return boost::make_tuple(handlers, updater);
    }

};

} // end anonymous namespace


int main(int argc, char* argv[]) {
    return ::nova::guest::agent::execute_main<Func, PeriodicTasksPtr>(
        "MySQL Edition", argc, argv);
}
