# Unless explicitly stated otherwise all files in this repository are licensed
# under the Apache License Version 2.0.
# This product includes software developed at Datadog (https://www.datadoghq.com/).
# Copyright 2016-present Datadog, Inc.

# Datadog Agent install script for macOS.
set -e
install_script_version=1.0.0
dmg_file=/tmp/datadog-agent.dmg
dmg_base_url="https://s3.amazonaws.com/dd-agent"
etc_dir=/opt/datadog-agent/etc
service_name="com.datadoghq.agent"
systemwide_servicefile_name="/Library/LaunchDaemons/${service_name}.plist"

upgrade=
if [ -n "$DD_UPGRADE" ]; then
    upgrade=$DD_UPGRADE
fi

# Root user detection
if [ "$(echo "$UID")" = "0" ]; then
    sudo_cmd=''
else
    sudo_cmd='sudo'
fi

apikey=
if [ -n "$DD_API_KEY" ]; then
    apikey=$DD_API_KEY
fi

site=
if [ -n "$DD_SITE" ]; then
    site=$DD_SITE
fi

systemdaemon_install=false
systemdaemon_user_group=
if [ -n "$DD_SYSTEMDAEMON_INSTALL" ]; then
    systemdaemon_install=$DD_SYSTEMDAEMON_INSTALL
    if [ -n "$DD_SYSTEMDAEMON_USER_GROUP" ]; then
        systemdaemon_user_group=$DD_SYSTEMDAEMON_USER_GROUP
    else
        printf "\033[31mDD_SYSTEMDAEMON_INSTALL set without DD_SYSTEDAEMON_USER_GROUP\033[0m\n"
        exit 1;
    fi
    if ! echo "$systemdaemon_user_group" | grep "^[^:][^:]*:[^:][^:]*$" > /dev/null; then
        printf "\033[31mDD_SYSTEDAEMON_USER_GROUP must be in format UID:GID or UserName:GroupName\033[0m\n"
        exit 1;
    fi
fi

agent_major_version=6
if [ -n "$DD_AGENT_MAJOR_VERSION" ]; then
  if [ "$DD_AGENT_MAJOR_VERSION" != "6" ] && [ "$DD_AGENT_MAJOR_VERSION" != "7" ]; then
    echo "DD_AGENT_MAJOR_VERSION must be either 6 or 7. Current value: $DD_AGENT_MAJOR_VERSION"
    exit 1;
  fi
  agent_major_version=$DD_AGENT_MAJOR_VERSION
else
  echo -e "\033[33mWarning: DD_AGENT_MAJOR_VERSION not set. Installing Agent version 6 by default.\033[0m"
fi

dmg_remote_file="datadogagent.dmg"
if [ "$agent_major_version" = "7" ]; then
    dmg_remote_file="datadog-agent-7-latest.dmg"
fi
dmg_url="$dmg_base_url/$dmg_remote_file"

if [ "$upgrade" ]; then
    if [ ! -f $etc_dir/datadog.conf ]; then
        printf "\033[31mDD_UPGRADE set but no config was found at $etc_dir/datadog.conf.\033[0m\n"
        exit 1;
    fi
fi

if [ ! "$apikey" ]; then
    # if it's an upgrade, then we will use the transition script
    if [ ! "$upgrade" ]; then
        printf "\033[31mAPI key not available in DD_API_KEY environment variable.\033[0m\n"
        exit 1;
    fi
fi


# SUDO_USER is defined in man sudo: https://linux.die.net/man/8/sudo
# "SUDO_USER Set to the login name of the user who invoked sudo."

# USER is defined in man login: https://ss64.com/osx/login.html
# "Login enters information into the environment (see environ(7))
#  specifying the user's home directory (HOME), command interpreter (SHELL),
#  search path (PATH), terminal type (TERM) and user name (both LOGNAME and USER)."

# We want to get the real user who executed the command. Two situations can happen:
# - the command was run as the current user: then $USER contains the user which launched the command, and $SUDO_USER is empty,
# - the command was run with sudo: then $USER contains the name of the user targeted by the sudo command (by default, root)
#   and $SUDO_USER contains the user which launched the sudo command.
# The following block covers both cases so that we have tbe username we want in the real_user variable.
real_user=`if [ "$SUDO_USER" ]; then
  echo "$SUDO_USER"
else
  echo "$USER"
fi`

TMPDIR=`sudo -u "$real_user" getconf DARWIN_USER_TEMP_DIR`
export TMPDIR

cmd_real_user="sudo -Eu $real_user"

user_home=$($cmd_real_user bash -c 'echo "$HOME"')
user_plist_file=${user_home}/Library/LaunchAgents/${service_name}.plist

# In order to install with the right user
rm -f /tmp/datadog-install-user
echo "$real_user" > /tmp/datadog-install-user

function on_error() {
    printf "\033[31m$ERROR_MESSAGE
It looks like you hit an issue when trying to install the Agent.

Troubleshooting and basic usage information for the Agent are available at:

    https://docs.datadoghq.com/agent/basic_agent_usage/

If you're still having problems, please send an email to support@datadoghq.com
with the contents of ddagent-install.log and we'll do our very best to help you
solve your problem.\n\033[0m\n"
}
trap on_error ERR

cmd_agent="$cmd_real_user /opt/datadog-agent/bin/agent/agent"

cmd_launchctl="$cmd_real_user launchctl"

function new_config() {
    # Check for vanilla OS X sed or GNU sed
    i_cmd="-i ''"
    if [ "$(sed --version 2>/dev/null | grep -c "GNU")" -ne 0 ]; then i_cmd="-i"; fi
    $sudo_cmd sh -c "sed $i_cmd 's/api_key:.*/api_key: $apikey/' \"$etc_dir/datadog.yaml\""
    if [ "$site" ]; then
        $sudo_cmd sh -c "sed $i_cmd 's/# site:.*/site: $site/' \"$etc_dir/datadog.yaml\""
    fi
    $sudo_cmd chown "$real_user":admin "$etc_dir/datadog.yaml"
    $sudo_cmd chmod 640 $etc_dir/datadog.yaml
}

function import_config() {
    printf "\033[34m\n* Converting old datadog.conf file to new datadog.yaml format\n\033[0m\n"
    $cmd_agent import $etc_dir $etc_dir -f
}

function plist_modify_user_group() {
    plist_file="$1"
    user_value="$(echo $2 | awk -F: '{ print $1 }')"
    group_value="$(echo $2 | awk -F: '{ print $2 }')"
    user_parameter="UID"
    group_parameter="GID"

    # we have to distinguish between uid/gid and username/groupname
    if [ ! -z "${user_value##[0-9]*}" ]; then
        user_parameter="UserName"
    fi
    if [ ! -z "${group_value##[0-9]*}" ]; then
        group_parameter="GroupName"
    fi

    # if, in a future agent version we add UID/GID/UserName/GroupName to the plist file,
    # we want this older version of install script fail, because it wouldn't know what to do
    terms="UID UserName GID GroupName"
    for term in $terms; do
        if grep "<key>$term</key>" "$1"; then
            printf "\033[31m$plist_file already contains <key>$term</key>, please update this script to the latest version\033[0m\n"
            return 1
        fi
    done

    ## to insert user/group into the xml file, we'll find the last "</dict>" occurence and insert before it
    closing_dict_line=$($sudo_cmd cat "$plist_file" | grep -n "</dict>" | tail -1 | cut -f1 -d:)
    # there's no way to do in-place sed without a backup file on an arbitrary MacOS version
    $sudo_cmd sed -i .backup -e "${closing_dict_line},${closing_dict_line}s|</dict>|<key>$user_parameter</key><string>$user_value</string>\n</dict>|" -e "${closing_dict_line},${closing_dict_line}s|</dict>|<key>$group_parameter</key><string>$group_value</string>\n</dict>|" "$plist_file"
    $sudo_cmd rm "${plist_file}.backup"
}

# # Install the agent
printf "\033[34m\n* Downloading datadog-agent\n\033[0m"
rm -f $dmg_file
# curl --fail --progress-bar $dmg_url > $dmg_file
dmg_file=/Users/slavek.kabrda/Downloads/datadog-agent-7.34.0-rc.1-1.dmg
printf "\033[34m\n* Installing datadog-agent, you might be asked for your sudo password...\n\033[0m"
$sudo_cmd hdiutil detach "/Volumes/datadog_agent" >/dev/null 2>&1 || true
printf "\033[34m\n    - Mounting the DMG installer...\n\033[0m"
$sudo_cmd hdiutil attach "$dmg_file" -mountpoint "/Volumes/datadog_agent" >/dev/null
if [ "$systemdaemon_install" != false ] && [ -f "$systemwide_servicefile_name" ]; then
    # TODO: if systemwide is running, but user is trying to install locally, fail
    printf "\033[34m\n    - Stopping systemwide Datadog Agent daemon ...\n\033[0m"
    # we use "|| true" because if the service is not started/loaded, the commands fail
    $sudo_cmd launchctl stop $service_name || true
    $sudo_cmd launchctl print system/$service_name 2>/dev/null >/dev/null && $sudo_cmd launchctl unload $systemwide_servicefile_name || true
fi
printf "\033[34m\n    - Unpacking and copying files (this usually takes about a minute) ...\n\033[0m"
cd / && $sudo_cmd /usr/sbin/installer -pkg "`find "/Volumes/datadog_agent" -name \*.pkg 2>/dev/null`" -target / >/dev/null
printf "\033[34m\n    - Unmounting the DMG installer ...\n\033[0m"
$sudo_cmd hdiutil detach "/Volumes/datadog_agent" >/dev/null

# Creating or overriding the install information
install_info_content="---
install_method:
  tool: install_script
  tool_version: install_script_mac
  installer_version: install_script_mac-$install_script_version
"
$sudo_cmd sh -c "echo '$install_info_content' > $etc_dir/install_info"
$sudo_cmd chown "$real_user":admin "$etc_dir/install_info"
$sudo_cmd chmod 640 $etc_dir/install_info

# Set the configuration
if grep -E 'api_key:( APIKEY)?$' "$etc_dir/datadog.yaml" > /dev/null 2>&1; then
    if [ "$upgrade" ]; then
        import_config
    else
        new_config
    fi
    printf "\n\033[34m* Restarting the Agent...\n\033[0m\n"
    $cmd_launchctl stop $service_name

    # Wait for the agent to fully stop
    retry=0
    until [ "$retry" -ge 5 ]; do
        curl -m 5 -o /dev/null -s -I http://127.0.0.1:5002 || break
        retry=$[$retry+1]
        sleep 5
    done
    if [ "$retry" -ge 5 ]; then
        printf "\n\033[33mCould not restart the agent.
You may have to restart it manually using the systray app or the
\"launchctl start com.datadoghq.agent\" command.\n\033[0m\n"
    fi

    $cmd_launchctl start $service_name
else
    printf "\033[34m\n* A datadog.yaml configuration file already exists. It will not be overwritten.\n\033[0m\n"
fi

# Starting the app
if [ "$systemdaemon_install" = false ]; then
    $cmd_real_user open -a 'Datadog Agent.app'
else
    # TODO: modify the message below
    printf "\033[34m\n* Installing $service_name as a systemwide LaunchDaemon ...\n\n\033[0m"
    # disable launching the System Tray app, which is enabled in the postinstall package script
    $cmd_real_user osascript -e 'tell application "System Events" to if login item "Datadog Agent" exists then delete login item "Datadog Agent"'
    # unload the agent for current user
    $cmd_launchctl stop "$service_name"
    $cmd_launchctl unload "$user_plist_file"
    # move the plist file to the system location
    $sudo_cmd mv "$user_plist_file" /Library/LaunchDaemons/
    # make sure the daemon launches under proper user/group and then start it
    plist_modify_user_group "$systemwide_servicefile_name" "$systemdaemon_user_group"
    $sudo_cmd chown "$systemdaemon_user_group" "$systemwide_servicefile_name"
    $sudo_cmd launchctl load -w "$systemwide_servicefile_name"
    $sudo_cmd launchctl start "$service_name"
fi

# Agent works, echo some instructions and exit
printf "\033[32m

Your Agent is running properly. It will continue to run in the
background and submit metrics to Datadog.

You can check the agent status using the \"datadog-agent status\" command
or by opening the webui using the \"datadog-agent launch-gui\" command.

If you ever want to stop the Agent, please use the Datadog Agent App or
the launchctl command. It will start automatically at login.

\033[0m"
