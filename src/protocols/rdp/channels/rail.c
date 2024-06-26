/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "channels/rail.h"
#include "plugins/channels.h"
#include "rdp.h"
#include "settings.h"

#include <freerdp/client/rail.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/rail.h>
#include <guacamole/client.h>
#include <winpr/wtypes.h>
#include <winpr/wtsapi.h>

#include <stddef.h>
#include <string.h>

#ifdef FREERDP_RAIL_CALLBACKS_REQUIRE_CONST
/**
 * FreeRDP 2.0.0-rc4 and newer requires the final argument for all RAIL
 * callbacks to be const.
 */
#define RAIL_CONST const
#else
/**
 * FreeRDP 2.0.0-rc3 and older requires the final argument for all RAIL
 * callbacks to NOT be const.
 */
#define RAIL_CONST
#endif

/**
 * Completes initialization of the RemoteApp session, responding to the server
 * handshake, sending client status and system parameters, and executing the
 * desired RemoteApp command. This is accomplished using the Handshake PDU,
 * Client Information PDU, one or more Client System Parameters Update PDUs,
 * and the Client Execute PDU respectively. These PDUs MUST be sent for the
 * desired RemoteApp to run, and MUST NOT be sent until after a Handshake or
 * HandshakeEx PDU has been received. See:
 *
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/cec4eb83-b304-43c9-8378-b5b8f5e7082a (Handshake PDU)
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/743e782d-f59b-40b5-a0f3-adc74e68a2ff (Client Information PDU)
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/60344497-883f-4711-8b9a-828d1c580195 (System Parameters Update PDU)
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/98a6e3c3-c2a9-42cc-ad91-0d9a6c211138 (Client Execute PDU)
 *
 * @param rail
 *     The RailClientContext structure used by FreeRDP to handle the RAIL
 *     channel for the current RDP session.
 *
 * @return
 *     CHANNEL_RC_OK (zero) if the PDUs were sent successfully, an error code
 *     (non-zero) otherwise.
 */
static UINT guac_rdp_rail_complete_handshake(RailClientContext* rail) {

    UINT status;

    guac_client* client = (guac_client*) rail->custom;
    guac_rdp_client* rdp_client = (guac_rdp_client*) client->data;

    RAIL_HANDSHAKE_ORDER handshake = {

        /* Build number 7600 (0x1DB0) apparently represents Windows 7 and
         * compatibility with RDP 7.0. As of this writing, this is the same
         * build number sent for RAIL connections by xfreerdp. */
        .buildNumber = 7600

    };

    /* Send client handshake response */
    pthread_mutex_lock(&(rdp_client->message_lock));
    status = rail->ClientHandshake(rail, &handshake);
    pthread_mutex_unlock(&(rdp_client->message_lock));

    if (status != CHANNEL_RC_OK)
        return status;

    RAIL_CLIENT_STATUS_ORDER client_status = {
        .flags = 0x00
    };

    /* Send client status */
    pthread_mutex_lock(&(rdp_client->message_lock));
    status = rail->ClientInformation(rail, &client_status);
    pthread_mutex_unlock(&(rdp_client->message_lock));

    if (status != CHANNEL_RC_OK)
        return status;

    RAIL_SYSPARAM_ORDER sysparam = {

        .dragFullWindows = FALSE,

        .highContrast = {
            .flags =
                  HCF_AVAILABLE
                | HCF_CONFIRMHOTKEY
                | HCF_HOTKEYACTIVE
                | HCF_HOTKEYAVAILABLE
                | HCF_HOTKEYSOUND
                | HCF_INDICATOR,
            .colorScheme = {
                .string = NULL,
                .length = 0
            }
        },

        .keyboardCues = FALSE,
        .keyboardPref = FALSE,
        .mouseButtonSwap = FALSE,

        .workArea = {
            .left   = 0,
            .top    = 0,
            .right  = rdp_client->settings->width,
            .bottom = rdp_client->settings->height
        },

        .params =
              SPI_MASK_SET_DRAG_FULL_WINDOWS
            | SPI_MASK_SET_HIGH_CONTRAST
            | SPI_MASK_SET_KEYBOARD_CUES
            | SPI_MASK_SET_KEYBOARD_PREF
            | SPI_MASK_SET_MOUSE_BUTTON_SWAP
            | SPI_MASK_SET_WORK_AREA

    };

    /* Send client system parameters */
    pthread_mutex_lock(&(rdp_client->message_lock));
    status = rail->ClientSystemParam(rail, &sysparam);
    pthread_mutex_unlock(&(rdp_client->message_lock));

    if (status != CHANNEL_RC_OK)
        return status;

    RAIL_EXEC_ORDER exec = {
        .flags = RAIL_EXEC_FLAG_EXPAND_ARGUMENTS,
        .RemoteApplicationProgram = rdp_client->settings->remote_app,
        .RemoteApplicationWorkingDir = rdp_client->settings->remote_app_dir,
        .RemoteApplicationArguments = rdp_client->settings->remote_app_args,
    };

    /* Execute desired RemoteApp command */
    pthread_mutex_lock(&(rdp_client->message_lock));
    status = rail->ClientExecute(rail, &exec);
    pthread_mutex_unlock(&(rdp_client->message_lock));

    return status;

}

/**
 * A callback function that is invoked when the RDP server sends the result
 * of the Remote App (RAIL) execution command back to the client, so that the
 * client can handle any required actions associated with the result.
 * 
 * @param context
 *     A pointer to the RAIL data structure associated with the current
 *     RDP connection.
 *
 * @param execResult
 *     A data structure containing the result of the RAIL command.
 *
 * @return
 *     CHANNEL_RC_OK (zero) if the result was handled successfully, otherwise
 *     a non-zero error code. This implementation always returns
 *     CHANNEL_RC_OK.
 */
static UINT guac_rdp_rail_execute_result(RailClientContext* context,
        const RAIL_EXEC_RESULT_ORDER* execResult) {

    guac_client* client = (guac_client*) context->custom;

    if (execResult->execResult != RAIL_EXEC_S_OK) {
        guac_client_log(client, GUAC_LOG_DEBUG, "Failed to execute RAIL command on server: %d", execResult->execResult);
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_UNAVAILABLE, "Failed to execute RAIL command.");
    }

    return CHANNEL_RC_OK;

}

/**
 * Callback which is invoked when a Handshake PDU is received from the RDP
 * server. No communication for RemoteApp may occur until the Handshake PDU
 * (or, alternatively, the HandshakeEx PDU) is received. See:
 *
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/cec4eb83-b304-43c9-8378-b5b8f5e7082a
 *
 * @param rail
 *     The RailClientContext structure used by FreeRDP to handle the RAIL
 *     channel for the current RDP session.
 *
 * @param handshake
 *     The RAIL_HANDSHAKE_ORDER structure representing the Handshake PDU that
 *     was received.
 *
 * @return
 *     CHANNEL_RC_OK (zero) if the PDU was handled successfully, an error code
 *     (non-zero) otherwise.
 */
static UINT guac_rdp_rail_handshake(RailClientContext* rail,
        RAIL_CONST RAIL_HANDSHAKE_ORDER* handshake) {
    return guac_rdp_rail_complete_handshake(rail);
}

/**
 * Callback which is invoked when a HandshakeEx PDU is received from the RDP
 * server. No communication for RemoteApp may occur until the HandshakeEx PDU
 * (or, alternatively, the Handshake PDU) is received. See:
 *
 * https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-rdperp/5cec5414-27de-442e-8d4a-c8f8b41f3899
 *
 * @param rail
 *     The RailClientContext structure used by FreeRDP to handle the RAIL
 *     channel for the current RDP session.
 *
 * @param handshake_ex
 *     The RAIL_HANDSHAKE_EX_ORDER structure representing the HandshakeEx PDU
 *     that was received.
 *
 * @return
 *     CHANNEL_RC_OK (zero) if the PDU was handled successfully, an error code
 *     (non-zero) otherwise.
 */
static UINT guac_rdp_rail_handshake_ex(RailClientContext* rail,
        RAIL_CONST RAIL_HANDSHAKE_EX_ORDER* handshake_ex) {
    return guac_rdp_rail_complete_handshake(rail);
}

/**
 * Callback which associates handlers specific to Guacamole with the
 * RailClientContext instance allocated by FreeRDP to deal with received
 * RAIL (RemoteApp) messages.
 *
 * This function is called whenever a channel connects via the PubSub event
 * system within FreeRDP, but only has any effect if the connected channel is
 * the RAIL channel. This specific callback is registered with the PubSub
 * system of the relevant rdpContext when guac_rdp_rail_load_plugin() is
 * called.
 *
 * @param context
 *     The rdpContext associated with the active RDP session.
 *
 * @param args
 *     Event-specific arguments, mainly the name of the channel, and a
 *     reference to the associated plugin loaded for that channel by FreeRDP.
 */
static void guac_rdp_rail_channel_connected(rdpContext* context,
        ChannelConnectedEventArgs* args) {

    guac_client* client = ((rdp_freerdp_context*) context)->client;

    /* Ignore connection event if it's not for the RAIL channel */
    if (strcmp(args->name, RAIL_SVC_CHANNEL_NAME) != 0)
        return;

    /* The structure pointed to by pInterface is guaranteed to be a
     * RailClientContext if the channel is RAIL */
    RailClientContext* rail = (RailClientContext*) args->pInterface;

    /* Init FreeRDP RAIL context, ensuring the guac_client can be accessed from
     * within any RAIL-specific callbacks */
    rail->custom = client;
    rail->ServerExecuteResult = guac_rdp_rail_execute_result;
    rail->ServerHandshake = guac_rdp_rail_handshake;
    rail->ServerHandshakeEx = guac_rdp_rail_handshake_ex;

    guac_client_log(client, GUAC_LOG_DEBUG, "RAIL (RemoteApp) channel "
            "connected.");

}

void guac_rdp_rail_load_plugin(rdpContext* context) {

    guac_client* client = ((rdp_freerdp_context*) context)->client;

    /* Attempt to load FreeRDP support for the RAIL channel */
    if (guac_freerdp_channels_load_plugin(context, "rail", context->settings)) {
        guac_client_log(client, GUAC_LOG_WARNING,
                "Support for the RAIL channel (RemoteApp) could not be "
                "loaded. This support normally takes the form of a plugin "
                "which is built into FreeRDP. Lacking this support, "
                "RemoteApp will not work.");
        return;
    }

    /* Complete RDP side of initialization when channel is connected */
    PubSub_SubscribeChannelConnected(context->pubSub,
            (pChannelConnectedEventHandler) guac_rdp_rail_channel_connected);

    guac_client_log(client, GUAC_LOG_DEBUG, "Support for RAIL (RemoteApp) "
            "registered. Awaiting channel connection.");

}

