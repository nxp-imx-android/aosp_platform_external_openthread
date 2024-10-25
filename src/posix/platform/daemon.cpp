/*
 *  Copyright (c) 2021, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "posix/platform/daemon.hpp"

#if defined(__ANDROID__) && !OPENTHREAD_CONFIG_ANDROID_NDK_ENABLE
#include <cutils/sockets.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <openthread/cli.h>

#include "cli/cli_config.h"
#include "common/code_utils.hpp"
#include "posix/platform/platform-posix.h"

#if OPENTHREAD_POSIX_CONFIG_DAEMON_ENABLE

#define OPENTHREAD_POSIX_DAEMON_SOCKET_LOCK OPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME ".lock"
static_assert(sizeof(OPENTHREAD_POSIX_DAEMON_SOCKET_NAME) < sizeof(sockaddr_un::sun_path),
              "OpenThread daemon socket name too long!");

namespace ot {
namespace Posix {

namespace {

typedef char(Filename)[sizeof(sockaddr_un::sun_path)];

void GetFilename(Filename &aFilename, const char *aPattern)
{
    int         rval;
    const char *netIfName = strlen(gNetifName) > 0 ? gNetifName : OPENTHREAD_POSIX_CONFIG_THREAD_NETIF_DEFAULT_NAME;

    rval = snprintf(aFilename, sizeof(aFilename), aPattern, netIfName);
    if (rval < 0 && static_cast<size_t>(rval) >= sizeof(aFilename))
    {
        DieNow(OT_EXIT_INVALID_ARGUMENTS);
    }
}

} // namespace

//Vendor Specific commands
extern "C"
{
#define MFG_CMD_ACTION_GET 0
#define MFG_CMD_ACTION_SET 1

#define MFG_CMD_GET_SET_CHANNEL 0x0b      // 11
#define MFG_CMD_GET_SET_TXPOWER 0x0f      // 15
#define MFG_CMD_CONTINUOUS_TX 0x11        // 17
#define MFG_CMD_GET_SET_PAYLOAD_SIZE 0x14 // 20
#define MFG_CMD_GET_RX_RESULT 0x1f        // 31
#define MFG_CMD_START_RX_TEST 0x20        // 32
#define MFG_CMD_BURST_TX 0x21             // 33
#define MFG_CMD_DUTY_CYCLE_TX 0x23        // 35
#define MFG_CMD_GET_SET_CCA_THRESHOLD  0x2F   // 47
#define MFG_CMD_CONTINOUS_CCA_TEST 0X31   //49
#define MFG_CMD_GET_CCA_STATUS 0x32       //50
#define MFG_CMD_CONTINOUS_ED_TEST 0x37    //55
#define MFG_CMD_GET_ED_VALUE    0x38      //56
#define MFG_CMD_PHY_TX_TEST_PSDU 0x39     //57
#define MFG_CMD_PHY_RX_TX_ACK_TEST 0x3A   //58
#define MFG_CMD_SET_GENERIC_PARAM 0x3B    //59

#define MAX_VERSION_STRING_SIZE 128 //< Max size of version string

static uint8_t mfgEnable = 0;

// 15.4_INDEPENDENT_RESET
otError ProcessIRConfig(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    //syslog(LOG_INFO, "ProcessIRConfig");
    otError error = OT_ERROR_INVALID_ARGS;
    uint8_t mode = 0;

    if( aArgsLength == 1 )
    {
        mode = (uint8_t)atoi(aArgs[0]);
        //syslog(LOG_INFO, "-> mode %s", mode==0 ? "Disable IR":mode==3 ? "OOB IR 15.4":(mode==1 ?("OOB IR"):"InBand IR"));
        if( mode < 4 )
        {
            otPlatRadioSetIRConfig((otInstance*)aContext, mode);
            //syslog(LOG_INFO, "ProcessIRConfig DONE");
            error = OT_ERROR_NONE;
        }
    }
    else
    {
        //syslog(LOG_INFO, "ProcessIRConfig FAILED!");
        otPlatRadioGetIRConfig((otInstance*)aContext, &mode);

        // Print value as ot-cli output
        otCliOutputFormat("%d\r\n", mode);

        error = OT_ERROR_NONE;
    }

    return error;
}

otError ProcessIRCmd(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    OT_UNUSED_VARIABLE(aArgsLength);
    OT_UNUSED_VARIABLE(aArgs);
    //syslog(LOG_INFO, "ProcessIRCmd");
    otPlatRadioSetIRCmd((otInstance*)aContext);
    //syslog(LOG_INFO, "ProcessIRCmd DONE");

    return OT_ERROR_NONE;
}

otError ProcessSetEui64(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    otError error = OT_ERROR_INVALID_ARGS;

    if( aArgsLength == 1 )
    {
        otExtAddress addr;
        char        *hex  = *aArgs;

        //syslog(LOG_INFO, "+ SetEui64 %s (len %d)", *aArgs, (uint32_t)strlen(*aArgs));

        if( (hex[1] == 'x') && (strlen(*aArgs) == 18) )
        {
            error = OT_ERROR_NONE;

            hex = hex + 2;

            for(uint32_t i = 0; (i < 8) && (error == OT_ERROR_NONE); i++)
            {
                addr.m8[i] = 0;
                for(uint32_t k = 0; k < 2; k++)
                {
                    // get current character then increment
                    uint8_t byte = *hex++;
                    // transform hex character to the 4bit equivalent number, using the ascii table indexes
                    if (byte >= '0' && byte <= '9')
                        byte = byte - '0';
                    else if (byte >= 'a' && byte <='f')
                        byte = byte - 'a' + 10;
                    else if (byte >= 'A' && byte <='F')
                        byte = byte - 'A' + 10;
                    else
                    {
                        error = OT_ERROR_FAILED;
                        break;
                    }
                    // shift 4 to make space for new digit, and add the 4 bits of the new digit
                    addr.m8[i] = (addr.m8[i] << 4) | (byte & 0xF);
                }
            }

            if( error == OT_ERROR_NONE )
            {
                error = otPlatRadioSetIeeeEui64((otInstance*)aContext, (const otExtAddress*)&addr);
            }

            if( error != OT_ERROR_NONE )
            {
                //syslog(LOG_INFO, "- SetEui64 Failed (%#x)", error);
            }
            else
            {
                //syslog(LOG_INFO, "- SetEui64 SUCCESS");
            }
        }
        else
        {
            //syslog(LOG_INFO, "- SetEui64 invalid input arg (0x....?) !");
        }
    }
    else
    {
        //syslog(LOG_INFO, "- SetEui64 FAILED !");
    }

    return error;
}

otError ProcessGetSetTxPowerLimit(void *aContext, uint8_t aArgsLength, char *aArgs[])
{

    otError error = OT_ERROR_INVALID_ARGS;
    uint8_t txPowerLimit = 0;

    //syslog(LOG_INFO, "SetTxPowerLimit");

    if( aArgsLength == 1 ) // set tx power limit
    {
        txPowerLimit = (uint8_t)atoi(aArgs[0]);
        if((txPowerLimit>=1)&&(txPowerLimit<=44)){
            //syslog(LOG_INFO, "-> txPowerLimit : %d", txPowerLimit);
        }else{
            //syslog(LOG_INFO, "-> txPowerLimit : default value");
        }
        otPlatRadioSetTxPowerLimit((otInstance*)aContext, txPowerLimit);
        //syslog(LOG_INFO, "SetTxPowerLimit DONE");
        error = OT_ERROR_NONE;
    }
    else if ( aArgsLength == 0 ) // get tx power limit
    {
        otPlatRadioGetTxPowerLimit((otInstance*)aContext, &txPowerLimit);

        // Add value in syslog
        //syslog(LOG_INFO, "TX power Value value : %d", txPowerLimit);

        // Print value as ot-cli output
        otCliOutputFormat("%d\r\n", txPowerLimit);
        error = OT_ERROR_NONE;
    }
    else
    {
        //syslog(LOG_INFO, "SetTxPowerLimit FAILED! Invalid input arg");
    }

    return error;
}

 otError ProcessMfgGetInt8(void *aContext, uint8_t cmdId, uint8_t aArgsLength)
{
    otError error = OT_ERROR_INVALID_ARGS;
    uint8_t outputLen = 0;
    uint8_t payload[12] = {11};
    uint8_t payloadLen = 12;

    if(aArgsLength == 1)
    {
        payload[1] = cmdId;
        payload[2] = MFG_CMD_ACTION_GET;

        otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);

        if((outputLen >= 5) && (payload[3] == 0))
        {
            if(cmdId == MFG_CMD_GET_SET_TXPOWER)
            {
                otCliOutputFormat("%d\r\n", ((int8_t)payload[4])/2);
            }
            else
            {
                otCliOutputFormat("%d\r\n", (int8_t)payload[4]);
            }
            error = OT_ERROR_NONE;
        }
        else{
            error = OT_ERROR_FAILED;
        }
    }

    return error;
}

 otError ProcessMfgSetInt8(void *aContext, uint8_t cmdId, uint8_t aArgsLength, char *aArgs[], int8_t min, int8_t max)
{
    otError error = OT_ERROR_INVALID_ARGS;
    uint8_t outputLen = 0;
    uint8_t payload[12] = {11};
    uint8_t payloadLen = 12;
    int8_t setValue = 0;

    if(aArgsLength == 2)
    {
        setValue = (int8_t)atoi(aArgs[1]);
        if((setValue >= min) && (setValue <= max))
        {
            payload[1] = cmdId;
            payload[2] = MFG_CMD_ACTION_SET;
            if(cmdId == MFG_CMD_GET_SET_TXPOWER)
            {
                payload[4] = ((uint8_t)setValue) << 1; // convert dBm to half dBm
            }
            else
            {
                payload[4] = (uint8_t)setValue;
            }

            otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);

            if((outputLen >= 4) && (payload[3] == 0))
            {
                error = OT_ERROR_NONE;
            }
            else
            {
                error = OT_ERROR_FAILED;
            }
        }
    }

    return error;
}

otError ProcessMfgCommands(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    uint8_t payload[12] = {11};
    uint8_t payloadLen = 12;
    uint8_t outputLen = 0;
    otError error = OT_ERROR_INVALID_ARGS;
    uint8_t cmdId, idx;

    if(aArgsLength == 1)
    {
        cmdId = (uint8_t)atoi(aArgs[0]);
        if((cmdId == 0)||(cmdId == 1))
        {
            mfgEnable = cmdId;
            //syslog(LOG_INFO, "MFG command SUCCESS");
            return OT_ERROR_NONE;
        }
    }

    if(mfgEnable == 0)
    {
        //syslog(LOG_INFO, "MFG command not enabled");
        otCliOutputFormat("MFG command not enabled. to enable it : mfgcmd 1\r\n");
        return OT_ERROR_NONE;
    }

    if ((aArgsLength > 0) && (mfgEnable == 1))
    {
        cmdId = (uint8_t)atoi(aArgs[0]);

        switch (cmdId)
        {
        case MFG_CMD_GET_SET_CHANNEL: // get channel
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_SET_CHANNEL, aArgsLength);
            break;

        case MFG_CMD_GET_SET_CHANNEL + 1: // set channel
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_GET_SET_CHANNEL, aArgsLength, aArgs, 11, 26);
            break;

        case MFG_CMD_GET_SET_TXPOWER: // get txpower
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_SET_TXPOWER, aArgsLength);
            break;

        case MFG_CMD_GET_SET_TXPOWER + 1: // set txpower
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_GET_SET_TXPOWER, aArgsLength, aArgs, -20, 22);
            break;

        case MFG_CMD_CONTINUOUS_TX:
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_CONTINUOUS_TX, aArgsLength, aArgs, 0, 1);
            break;

        case MFG_CMD_GET_SET_PAYLOAD_SIZE: // get
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_SET_PAYLOAD_SIZE, aArgsLength);
            break;

        case MFG_CMD_GET_SET_PAYLOAD_SIZE + 1: // set
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_GET_SET_PAYLOAD_SIZE, aArgsLength, aArgs, 17, 116);
            break;

        case MFG_CMD_GET_RX_RESULT:
        {
            if(aArgsLength == 1)
            {
                payload[1] = MFG_CMD_GET_RX_RESULT;
                payload[2] = MFG_CMD_ACTION_GET;
                otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                if(outputLen >= 11)
                {
                    otCliOutputFormat("status : %d\r\n", payload[4]);
                    otCliOutputFormat("rx_pkt_count : %d\r\n", payload[5]|(payload[6]<<8));
                    otCliOutputFormat("total_pkt_count : %d\r\n", payload[7]|(payload[8]<<8));
                    otCliOutputFormat("rssi : %d\r\n",(int8_t)payload[9]);
                    otCliOutputFormat("lqi : %d\r\n", payload[10]);
                    error = OT_ERROR_NONE;
                }
                else{
                    error = OT_ERROR_FAILED;
                }
            }
        }
        break;

        case MFG_CMD_START_RX_TEST:
        {
            if(aArgsLength == 1)
            {
                payload[1] = MFG_CMD_START_RX_TEST;
                otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                error = OT_ERROR_NONE;
            }
        }
        break;

        case MFG_CMD_BURST_TX:
        {
            uint8_t mode = 0, gap = 0;
            if(aArgsLength == 3)
            {
                mode = (uint8_t)atoi(aArgs[1]);
                gap = (uint8_t)atoi(aArgs[2]);
                if((mode < 8) && (gap > 5))
                {
                    payload[1] = MFG_CMD_BURST_TX;
                    payload[4] = mode;
                    payload[5] = gap;
                    otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                    error = OT_ERROR_NONE;
                }
            }
        }
        break;

        case MFG_CMD_DUTY_CYCLE_TX:
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_DUTY_CYCLE_TX, aArgsLength, aArgs, 0, 1);
            break;

        case MFG_CMD_GET_SET_CCA_THRESHOLD: // get
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_SET_CCA_THRESHOLD, aArgsLength);
            break;

        case MFG_CMD_GET_SET_CCA_THRESHOLD + 1: // set
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_GET_SET_CCA_THRESHOLD, aArgsLength, aArgs, -110, 0);
            break;

        case MFG_CMD_CONTINOUS_CCA_TEST:
        {
            if(aArgsLength == 3)
            {
                payload[1] = MFG_CMD_CONTINOUS_CCA_TEST;
                payload[2] = MFG_CMD_ACTION_SET;
                payload[4] = (uint8_t)atoi(aArgs[1]); // enable
                payload[5] = (uint8_t)atoi(aArgs[2]); // CCA Mode
                if((payload[4] < 2) && (payload[5] < 4))
                {
                    otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                    if((outputLen >= 4) && (payload[3] == 0))
                    {
                        error = OT_ERROR_NONE;
                    }
                    else{
                        error = OT_ERROR_FAILED;
                    }
                }
            }
        }
        break;

        case MFG_CMD_GET_CCA_STATUS: // get
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_CCA_STATUS, aArgsLength);
            break;

        case MFG_CMD_CONTINOUS_ED_TEST:
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_CONTINOUS_ED_TEST, aArgsLength, aArgs, 0, 1);
            break;

        case MFG_CMD_GET_ED_VALUE:
            error = ProcessMfgGetInt8((otInstance*)aContext, MFG_CMD_GET_ED_VALUE, aArgsLength);
            break;

        case MFG_CMD_PHY_TX_TEST_PSDU:
        {
            uint8_t count_opt, gap, ackEnable;
            if(aArgsLength == 4)
            {
                payload[1]  = MFG_CMD_PHY_TX_TEST_PSDU;
                payload[2]  = MFG_CMD_ACTION_SET;

                count_opt = (uint8_t)atoi(aArgs[1]);
                gap       = (uint8_t)atoi(aArgs[2]);
                ackEnable = (uint8_t)atoi(aArgs[3]);
                if((count_opt < 8) && (gap > 5) && (ackEnable < 2))
                {
                    payload[4]  = count_opt;
                    payload[5]  = gap;
                    payload[6]  = ackEnable;
                    otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                    error = OT_ERROR_NONE;
                }
            }
        }
        break;

        case MFG_CMD_PHY_RX_TX_ACK_TEST:
            error = ProcessMfgSetInt8((otInstance*)aContext, MFG_CMD_PHY_RX_TX_ACK_TEST, aArgsLength, aArgs, 0, 1);
            break;

        case MFG_CMD_SET_GENERIC_PARAM:
        {
            uint16_t panid, destaddr, srcaddr;
            if(aArgsLength == 5)
            {
                panid       = (uint16_t)strtol(aArgs[2], NULL, 16);
                destaddr    = (uint16_t)strtol(aArgs[3], NULL, 16);
                srcaddr     = (uint16_t)strtol(aArgs[4], NULL, 16);

                payload[1]  = MFG_CMD_SET_GENERIC_PARAM;
                payload[2]  = MFG_CMD_ACTION_SET;
                payload[4]  = (uint8_t) atoi(aArgs[1]); // SEQ_NUM
                payload[5]  = (uint8_t) (panid & 0xFF); // PAN ID LSB
                payload[6]  = (uint8_t) ((panid >> 8) & 0xFF); // PAN ID MSB
                payload[7]  = (uint8_t) (destaddr & 0xFF); // DEST ADDR LSB
                payload[8]  = (uint8_t) ((destaddr >> 8) & 0xFF); // DEST ADDR MSB
                payload[9]  = (uint8_t) (srcaddr & 0xFF); // SRC ADDR LSB
                payload[10] = (uint8_t) ((srcaddr >> 8) & 0xFF); // SRC ADDR MSB

                otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t*)payload, payloadLen, &outputLen);
                error = OT_ERROR_NONE;
            }
        }
        break;

        default:
            error = OT_ERROR_NOT_IMPLEMENTED;
            break;
        }
    }

    //HANDLE ERRORS
    if(error == OT_ERROR_NONE)
    {
        //syslog(LOG_INFO, "MFG command SUCCESS");
    }
    else if(aArgsLength == payloadLen)
    {
        // If user passed all the payload, this means this is a direct message for the RCP.
        // Send it and print the return results.
        for(idx = 0; idx < payloadLen; idx++)
        {
            payload[idx] = (uint8_t)atoi(aArgs[idx]);
        }
        otPlatRadioMfgCommand((otInstance*)aContext, (uint8_t *)payload, payloadLen, &outputLen);
        for(idx = 0; idx < outputLen; idx++)
        {
            otCliOutputFormat("%d ", payload[idx]);
        }
        otCliOutputFormat("\r\n");
        error = OT_ERROR_NONE;
        //syslog(LOG_INFO, "MFG command SUCCESS");
    }
    else if(error == OT_ERROR_INVALID_ARGS)
    {
        //syslog(LOG_INFO, "MFG command Invalid parameter");
        //otCliOutputFormat("INVALID PARAMETER\r\n");
    }
    else if(error == OT_ERROR_NOT_IMPLEMENTED)
    {
        //syslog(LOG_INFO, "MFG command not implemented");
        otCliOutputFormat("NOT IMPLEMENTED\r\n");
    }
    else
    {
        //syslog(LOG_INFO, "MFG command FAILED");
        otCliOutputFormat("FAILED\r\n");
    }

    return error;
}

otError ProcessGetSetCcaCfg(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    otCCAModeConfig aCcaCfg;
    //syslog(LOG_INFO, "SetCcaConfig");

    if( aArgsLength == 4 ) // set cca configuration
    {
        aCcaCfg.mCcaMode            = (uint8_t)strtol(aArgs[0], NULL, 16);
        aCcaCfg.mCca1Threshold      = (uint8_t)strtol(aArgs[1], NULL, 16);
        aCcaCfg.mCca2CorrThreshold  = (uint8_t)strtol(aArgs[2], NULL, 16);
        aCcaCfg.mCca2MinNumOfCorrTh = (uint8_t)strtol(aArgs[3], NULL, 16);
        if ((((aCcaCfg.mCcaMode >= 1) && (aCcaCfg.mCcaMode <= 4)) || (aCcaCfg.mCcaMode == 0xFF)) &&
           (aCcaCfg.mCca2MinNumOfCorrTh <= 6) ){
            otPlatRadioCcaConfigValue((otInstance*)aContext, &aCcaCfg, 0x1);
            //syslog(LOG_INFO, "SetCcaConfig DONE");
        }
    }
    else if ( aArgsLength == 0 ) // get cca configuration
    {
        otPlatRadioCcaConfigValue((otInstance*)aContext, &aCcaCfg, 0x0);

        // Add value in syslog
        //syslog(LOG_INFO, "CCA Configuration:\r\n");
        //syslog(LOG_INFO, "CCA Mode type: CCA1=0x01, CCA2=0x02, CCA3=0x03[CCA1 AND CCA2], CCA3=0x04[CCA1 OR CCA2], NoCCA=0xFF: 0x%x\r\n", aCcaCfg.mCcaMode);
        //syslog(LOG_INFO, "CCA1 Threshold Value : 0x%x\r\n", aCcaCfg.mCca1Threshold);
        //syslog(LOG_INFO, "CCA2 Correlation Threshold Value : 0x%x\r\n", aCcaCfg.mCca2CorrThreshold);
        //syslog(LOG_INFO, "CCA2 Minimim Number of Correlation Threshold Value : 0x%x\r\n", aCcaCfg.mCca2MinNumOfCorrTh);

        // Print value as ot-cli output
        otCliOutputFormat("CCA Configuration:\r\n");
        otCliOutputFormat("CCA Mode type: CCA1=0x01, CCA2=0x02, CCA3=0x03[CCA1 AND CCA2], CCA3=0x04[CCA1 OR CCA2], NoCCA=0xFF: 0x%x\r\n", aCcaCfg.mCcaMode);
        otCliOutputFormat("CCA1 Threshold Value : 0x%x\r\n", aCcaCfg.mCca1Threshold);
        otCliOutputFormat("CCA2 Correlation Threshold Value : 0x%x\r\n", aCcaCfg.mCca2CorrThreshold);
        otCliOutputFormat("CCA2 Minimim Number of Correlation Threshold Value : 0x%x\r\n", aCcaCfg.mCca2MinNumOfCorrTh);
    }
    else
    {
        //syslog(LOG_INFO, "ccacfg FAILED! Invalid input arg\r\nFormat: ccacfg <CcaMode> <Cca1Threshold> <Cca2CorrThreshold> <Cca2MinNumOfCorrTh>\r\nCcaMode: CCA Mode type [CCA1=0x01, CCA2=0x02, CCA3=0x03[CCA1 AND CCA2], CCA3=0x04[CCA1 OR CCA2], NoCCA=0xFF]\r\nCca1Threshold[1Byte Hex value]: Energy threshold for CCA Mode1\r\nCca2CorrThreshold[1Byte Hex value]: CCA Mode 2 Correlation Threshold\r\nCca2MinNumOfCorrTh: [0 to 6]\r\n");
        otCliOutputFormat("ccacfg FAILED! Invalid input arg\r\nFormat: ccacfg <CcaMode> <Cca1Threshold> <Cca2CorrThreshold> <Cca2MinNumOfCorrTh>\r\nCcaMode: CCA Mode type [CCA1=0x01, CCA2=0x02, CCA3=0x03[CCA1 AND CCA2], CCA3=0x04[CCA1 OR CCA2], NoCCA=0xFF]\r\nCca1Threshold[1Byte Hex value]: Energy threshold for CCA Mode1\r\nCca2CorrThreshold[1Byte Hex value]: CCA Mode 2 Correlation Threshold\r\nCca2MinNumOfCorrTh: [0 to 6]\r\n");
    }

    return OT_ERROR_NONE;
}

otError ProcessGetFwVersion(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    OT_UNUSED_VARIABLE(aArgs);

    if ( aArgsLength == 0 )
    {
        const char version[MAX_VERSION_STRING_SIZE] = {0};
        otPlatRadioGetFwVersionString((otInstance*)aContext, version, MAX_VERSION_STRING_SIZE);
        otCliOutputFormat("%s\r\n", version);
    }
    else
    {
        //syslog(LOG_INFO, "GetFwVersion FAILED! Invalid input arg");
    }
    return OT_ERROR_NONE;
}

otError ProcessGetSetIRThreshold(void *aContext, uint8_t aArgsLength, char *aArgs[])
{
    otIRConfig aIRConfig;
    //syslog(LOG_INFO, "ProcessGetSetIRThreshold");

    if( aArgsLength == 1 )      // Set IR threshold
    {
        aIRConfig.mIRThreshold = (uint16_t)atoi(aArgs[0]);
        if( ( aIRConfig.mIRThreshold >= 100 ) && ( aIRConfig.mIRThreshold<= 1000 ) )
        {
           otPlatRadioIRThresholdConfig((otInstance*)aContext, &aIRConfig, 0x01);
           //syslog(LOG_INFO, "OOB IR Threshold: %d\r\n", aIRConfig.mIRThreshold);
        }
        else
        {
           //syslog(LOG_INFO, "OOB IR Threshold FAILED! Invalid Threshold Time - Required[100 to 1000\r\n");
           otCliOutputFormat("OOB IR Threshold FAILED! Invalid Threshold Time - Required[100 to 1000\r\n");
        }
    }

    else if ( aArgsLength == 0 ) // get IR threshold
    {
        otPlatRadioIRThresholdConfig((otInstance*)aContext, &aIRConfig, 0x00);
        //syslog(LOG_INFO, "OOB IR Threshold: %d\r\n", aIRConfig.mIRThreshold);
        otCliOutputFormat("OOB IR Threshold: %d\r\n", aIRConfig.mIRThreshold);
    }

    else
    {
        //syslog(LOG_INFO, "OOB IR Threshold FAILED! Invalid input arg\r\nFormat: irthold <Threshold Time>\r\nThreshold Time : 100 to 1000\r\n");
        otCliOutputFormat("OOB IR Threshold FAILED! Invalid input arg\r\nFormat: irthold <Threshold Time>\r\nThreshold Time : 100 to 1000\r\n");
    }
    return OT_ERROR_NONE;
}

static const otCliCommand kCommands[] = {
    {"ircfg", ProcessIRConfig},    //=> OutOfBand Independent Reset Configuration ircfg <1> means OOB mode
    {"ircmd", ProcessIRCmd},       //=> InBand Independent Reset command
    {"seteui64", ProcessSetEui64}, //=> Set ieee.802.15.4 MAC Address
    {"txpwrlimit", ProcessGetSetTxPowerLimit}, //=> Set TX power limit for 15.4
    {"mfgcmd", ProcessMfgCommands}, //=> Generic VSC for MFG RF commands
    {"ccacfg", ProcessGetSetCcaCfg}, //=> Set/Get CCA configuration for 15.4 CCA Before Tx operation
    {"fwversion", ProcessGetFwVersion}, //=> Get firmware version for 15.4
    {"irthold", ProcessGetSetIRThreshold}   //=> OutOfBand Independent Reset Threshold configuration
};
} //extern "C"

const char Daemon::kLogModuleName[] = "Daemon";

int Daemon::OutputFormat(const char *aFormat, ...)
{
    int     ret;
    va_list ap;

    va_start(ap, aFormat);
    ret = OutputFormatV(aFormat, ap);
    va_end(ap);

    return ret;
}

int Daemon::OutputFormatV(const char *aFormat, va_list aArguments)
{
    static constexpr char truncatedMsg[] = "(truncated ...)";
    char                  buf[OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH];
    int                   rval;

    static_assert(sizeof(truncatedMsg) < OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH,
                  "OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH is too short!");

    rval = vsnprintf(buf, sizeof(buf), aFormat, aArguments);
    VerifyOrExit(rval >= 0, LogWarn("Failed to format CLI output: %s", strerror(errno)));

    if (rval >= static_cast<int>(sizeof(buf)))
    {
        rval = static_cast<int>(sizeof(buf) - 1);
        memcpy(buf + sizeof(buf) - sizeof(truncatedMsg), truncatedMsg, sizeof(truncatedMsg));
    }

    VerifyOrExit(mSessionSocket != -1);

#ifdef __linux__
    // Don't die on SIGPIPE
    rval = send(mSessionSocket, buf, static_cast<size_t>(rval), MSG_NOSIGNAL);
#else
    rval = static_cast<int>(write(mSessionSocket, buf, static_cast<size_t>(rval)));
#endif

    if (rval < 0)
    {
        LogWarn("Failed to write CLI output: %s", strerror(errno));
        close(mSessionSocket);
        mSessionSocket = -1;
    }

exit:
    return rval;
}

void Daemon::InitializeSessionSocket(void)
{
    int newSessionSocket;
    int rval;

    VerifyOrExit((newSessionSocket = accept(mListenSocket, nullptr, nullptr)) != -1, rval = -1);

    VerifyOrExit((rval = fcntl(newSessionSocket, F_GETFD, 0)) != -1);

    rval |= FD_CLOEXEC;

    VerifyOrExit((rval = fcntl(newSessionSocket, F_SETFD, rval)) != -1);

#ifndef __linux__
    // some platforms (macOS, Solaris) don't have MSG_NOSIGNAL
    // SOME of those (macOS, but NOT Solaris) support SO_NOSIGPIPE
    // if we have SO_NOSIGPIPE, then set it. Otherwise, we're going
    // to simply ignore it.
#if defined(SO_NOSIGPIPE)
    rval = setsockopt(newSessionSocket, SOL_SOCKET, SO_NOSIGPIPE, &rval, sizeof(rval));
    VerifyOrExit(rval != -1);
#else
#warning "no support for MSG_NOSIGNAL or SO_NOSIGPIPE"
#endif
#endif // __linux__

    if (mSessionSocket != -1)
    {
        close(mSessionSocket);
    }
    mSessionSocket = newSessionSocket;

exit:
    if (rval == -1)
    {
        LogWarn("Failed to initialize session socket: %s", strerror(errno));
        if (newSessionSocket != -1)
        {
            close(newSessionSocket);
        }
    }
    else
    {
        LogInfo("Session socket is ready");
    }
}

#if defined(__ANDROID__) && !OPENTHREAD_CONFIG_ANDROID_NDK_ENABLE
void Daemon::createListenSocketOrDie(void)
{
    Filename socketFile;

    // Don't use OPENTHREAD_POSIX_DAEMON_SOCKET_NAME because android_get_control_socket
    // below already assumes parent /dev/socket dir
    GetFilename(socketFile, "ot-daemon/%s.sock");

    // This returns the init-managed stream socket which is already bind to
    // /dev/socket/ot-daemon/<interface-name>.sock
    mListenSocket = android_get_control_socket(socketFile);

    if (mListenSocket == -1)
    {
        DieNowWithMessage("android_get_control_socket", OT_EXIT_ERROR_ERRNO);
    }
}
#else
void Daemon::createListenSocketOrDie(void)
{
    struct sockaddr_un sockname;
    int ret;

    class AllowAllGuard
    {
    public:
        AllowAllGuard(void)
        {
            const char *allowAll = getenv("OT_DAEMON_ALLOW_ALL");
            mAllowAll = (allowAll != nullptr && strcmp("1", allowAll) == 0);

            if (mAllowAll)
            {
                mMode = umask(0);
            }
        }
        ~AllowAllGuard(void)
        {
            if (mAllowAll)
            {
                umask(mMode);
            }
        }

    private:
        bool mAllowAll = false;
        mode_t mMode = 0;
    };

    mListenSocket = SocketWithCloseExec(AF_UNIX, SOCK_STREAM, 0, kSocketNonBlock);

    if (mListenSocket == -1)
    {
        DieNow(OT_EXIT_FAILURE);
    }

    {
        static_assert(sizeof(OPENTHREAD_POSIX_DAEMON_SOCKET_LOCK) == sizeof(OPENTHREAD_POSIX_DAEMON_SOCKET_NAME),
                      "sock and lock file name pattern should have the same length!");
        Filename lockfile;

        GetFilename(lockfile, OPENTHREAD_POSIX_DAEMON_SOCKET_LOCK);

        mDaemonLock = open(lockfile, O_CREAT | O_RDONLY | O_CLOEXEC, 0600);
    }

    if (mDaemonLock == -1)
    {
        DieNowWithMessage("open", OT_EXIT_ERROR_ERRNO);
    }

    if (flock(mDaemonLock, LOCK_EX | LOCK_NB) == -1)
    {
        DieNowWithMessage("flock", OT_EXIT_ERROR_ERRNO);
    }

    memset(&sockname, 0, sizeof(struct sockaddr_un));

    sockname.sun_family = AF_UNIX;
    GetFilename(sockname.sun_path, OPENTHREAD_POSIX_DAEMON_SOCKET_NAME);
    (void)unlink(sockname.sun_path);

    {
        AllowAllGuard allowAllGuard;

        ret = bind(mListenSocket, reinterpret_cast<const struct sockaddr *>(&sockname), sizeof(struct sockaddr_un));
    }

    if (ret == -1)
    {
        DieNowWithMessage("bind", OT_EXIT_ERROR_ERRNO);
    }
}
#endif // defined(__ANDROID__) && !OPENTHREAD_CONFIG_ANDROID_NDK_ENABLE

void Daemon::SetUp(void)
{
    int ret;

    // This allows implementing pseudo reset.
    VerifyOrExit(mListenSocket == -1);
    createListenSocketOrDie();

    //
    // only accept 1 connection.
    //
    ret = listen(mListenSocket, 1);
    if (ret == -1)
    {
        DieNowWithMessage("listen", OT_EXIT_ERROR_ERRNO);
    }

exit:
#if OPENTHREAD_POSIX_CONFIG_DAEMON_CLI_ENABLE
    otSysCliInitUsingDaemon(gInstance);
#endif

    otCliSetUserCommands(kCommands, OT_ARRAY_LENGTH(kCommands), gInstance);

    Mainloop::Manager::Get().Add(*this);

    return;
}

void Daemon::TearDown(void)
{
    Mainloop::Manager::Get().Remove(*this);

    if (mSessionSocket != -1)
    {
        close(mSessionSocket);
        mSessionSocket = -1;
    }

#if !defined(__ANDROID__) || OPENTHREAD_CONFIG_ANDROID_NDK_ENABLE
    // The `mListenSocket` is managed by `init` on Android
    if (mListenSocket != -1)
    {
        close(mListenSocket);
        mListenSocket = -1;
    }

    if (gPlatResetReason != OT_PLAT_RESET_REASON_SOFTWARE)
    {
        Filename sockfile;

        GetFilename(sockfile, OPENTHREAD_POSIX_DAEMON_SOCKET_NAME);
        LogDebg("Removing daemon socket: %s", sockfile);
        (void)unlink(sockfile);
    }

    if (mDaemonLock != -1)
    {
        (void)flock(mDaemonLock, LOCK_UN);
        close(mDaemonLock);
        mDaemonLock = -1;
    }
#endif
}

void Daemon::Update(otSysMainloopContext &aContext)
{
    if (mListenSocket != -1)
    {
        FD_SET(mListenSocket, &aContext.mReadFdSet);
        FD_SET(mListenSocket, &aContext.mErrorFdSet);

        if (aContext.mMaxFd < mListenSocket)
        {
            aContext.mMaxFd = mListenSocket;
        }
    }

    if (mSessionSocket != -1)
    {
        FD_SET(mSessionSocket, &aContext.mReadFdSet);
        FD_SET(mSessionSocket, &aContext.mErrorFdSet);

        if (aContext.mMaxFd < mSessionSocket)
        {
            aContext.mMaxFd = mSessionSocket;
        }
    }

    return;
}

void Daemon::Process(const otSysMainloopContext &aContext)
{
    ssize_t rval;

    VerifyOrExit(mListenSocket != -1);

    if (FD_ISSET(mListenSocket, &aContext.mErrorFdSet))
    {
        DieNowWithMessage("daemon socket error", OT_EXIT_FAILURE);
    }
    else if (FD_ISSET(mListenSocket, &aContext.mReadFdSet))
    {
        InitializeSessionSocket();
    }

    VerifyOrExit(mSessionSocket != -1);

    if (FD_ISSET(mSessionSocket, &aContext.mErrorFdSet))
    {
        close(mSessionSocket);
        mSessionSocket = -1;
    }
    else if (FD_ISSET(mSessionSocket, &aContext.mReadFdSet))
    {
        uint8_t buffer[OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH];

        // leave 1 byte for the null terminator
        rval = read(mSessionSocket, buffer, sizeof(buffer) - 1);

        if (rval > 0)
        {
            buffer[rval] = '\0';
#if OPENTHREAD_POSIX_CONFIG_DAEMON_CLI_ENABLE
            otCliInputLine(reinterpret_cast<char *>(buffer));
#else
            OutputFormat("Error: CLI is disabled!\n");
#endif
        }
        else
        {
            if (rval < 0)
            {
                LogWarn("Daemon read: %s", strerror(errno));
            }
            close(mSessionSocket);
            mSessionSocket = -1;
        }
    }

exit:
    return;
}

Daemon &Daemon::Get(void)
{
    static Daemon sInstance;

    return sInstance;
}

} // namespace Posix
} // namespace ot
#endif // OPENTHREAD_POSIX_CONFIG_DAEMON_ENABLE
