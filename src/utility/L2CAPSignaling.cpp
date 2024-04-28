/*
  This file is part of the ArduinoBLE library.
  Copyright (c) 2018 Arduino SA. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "HCI.h"
#include "ATT.h"
#include "btct.h"
#include "L2CAPSignaling.h"
#include "keyDistribution.h"
#include "bitDescriptions.h"

#define CONNECTION_PARAMETER_UPDATE_REQUEST  0x12
#define CONNECTION_PARAMETER_UPDATE_RESPONSE 0x13

//#define _BLE_TRACE_

L2CAPSignalingClass::L2CAPSignalingClass() :
        _minInterval(0),
        _maxInterval(0),
        _supervisionTimeout(0),
        _pairing_enabled(1) {
}

L2CAPSignalingClass::~L2CAPSignalingClass() {
}

void L2CAPSignalingClass::addConnection(uint16_t handle, uint8_t role, uint8_t /*peerBdaddrType*/,
                                        uint8_t /*peerBdaddr*/[6], uint16_t interval,
                                        uint16_t /*latency*/, uint16_t supervisionTimeout,
                                        uint8_t /*masterClockAccuracy*/) {
    if (role != 1) {
        // ignore
        return;
    }

    bool updateParameters = false;
    uint16_t updatedMinInterval = interval;
    uint16_t updatedMaxInterval = interval;
    uint16_t updatedSupervisionTimeout = supervisionTimeout;

    if (_minInterval && _maxInterval) {
        if (interval < _minInterval || interval > _maxInterval) {
            updatedMinInterval = _minInterval;
            updatedMaxInterval = _maxInterval;
            updateParameters = true;
        }
    }

    if (_supervisionTimeout && supervisionTimeout != _supervisionTimeout) {
        updatedSupervisionTimeout = _supervisionTimeout;
        updateParameters = true;
    }

    if (updateParameters) {
        struct __attribute__ ((packed)) L2CAPConnectionParameterUpdateRequest {
            uint8_t code;
            uint8_t identifier;
            uint16_t length;
            uint16_t minInterval;
            uint16_t maxInterval;
            uint16_t latency;
            uint16_t supervisionTimeout;
        } request = {CONNECTION_PARAMETER_UPDATE_REQUEST, 0x01, 8,
                     updatedMinInterval, updatedMaxInterval, 0x0000, updatedSupervisionTimeout};

        HCI.sendAclPkt(handle, SIGNALING_CID, sizeof(request), &request);
    }
}

void L2CAPSignalingClass::handleData(uint16_t connectionHandle, uint8_t dlen, uint8_t data[]) {
    struct __attribute__ ((packed)) L2CAPSignalingHdr {
        uint8_t code;
        uint8_t identifier;
        uint16_t length;
    } *l2capSignalingHdr = (L2CAPSignalingHdr *) data;

    if (dlen < sizeof(L2CAPSignalingHdr)) {
        // too short, ignore
        return;
    }

    if (dlen != (sizeof(L2CAPSignalingHdr) + l2capSignalingHdr->length)) {
        // invalid length, ignore
        return;
    }

    uint8_t code = l2capSignalingHdr->code;
    uint8_t identifier = l2capSignalingHdr->identifier;
    uint16_t length = l2capSignalingHdr->length;
    data = &data[sizeof(L2CAPSignalingHdr)];

    if (code == CONNECTION_PARAMETER_UPDATE_REQUEST) {
        connectionParameterUpdateRequest(connectionHandle, identifier, length, data);
    } else if (code == CONNECTION_PARAMETER_UPDATE_RESPONSE) {
        connectionParameterUpdateResponse(connectionHandle, identifier, length, data);
    }
}

void L2CAPSignalingClass::handleSecurityData(uint16_t connectionHandle, uint8_t dlen, uint8_t data[]) {
    struct __attribute__ ((packed)) L2CAPSignalingHdr {
        uint8_t code;
        uint8_t data[64];
    } *l2capSignalingHdr = (L2CAPSignalingHdr *) data;
#ifdef _BLE_TRACE_
    Serial.print("dlen: ");
    Serial.println(dlen);
#endif
    uint8_t code = l2capSignalingHdr->code;

#ifdef _BLE_TRACE_
    Serial.print("handleSecurityData: code: 0x");
    Serial.println(code, HEX);
    Serial.print("rx security:");
    btct.printBytes(data,dlen);
#endif
    if (code == CONNECTION_PAIRING_REQUEST) {

            handlePairingRequestFromInitiator(connectionHandle, l2capSignalingHdr);
    }
    else if (code == CONNECTION_PAIRING_RANDOM) {

        if (ATT.getPeerPairingInitiatorRelationship(connectionHandle))
        {
            struct __attribute__ ((packed)) PairingRandom {
                uint8_t Na[16];
            } *pairingRandom = (PairingRandom *) l2capSignalingHdr->data;
            for (int i = 0; i < 16; i++) {
                HCI.Na[15 - i] = pairingRandom->Na[i];
            }
#ifdef _BLE_TRACE_
            Serial.println("[Info] Pairing random.");
#endif
            struct __attribute__ ((packed)) PairingResponse {
                uint8_t code;
                uint8_t Nb[16];
            } response = {CONNECTION_PAIRING_RANDOM, 0};
            for (int i = 0; i < 16; i++) response.Nb[15 - i] = HCI.Nb[i];

            HCI.sendAclPkt(connectionHandle, SECURITY_CID, sizeof(response), &response);

            // We now have all needed for compare value
            uint8_t g2Result[4];
            uint8_t U[32];
            uint8_t V[32];

            for (int i = 0; i < 32; i++) {
                U[31 - i] = HCI.remotePublicKeyBuffer[i];
                V[31 - i] = HCI.localPublicKeyBuffer[i];
            }

            btct.g2(U, V, HCI.Na, HCI.Nb, g2Result);
            uint32_t result = 0;
            for (int i = 0; i < 4; i++) result += g2Result[3 - i] << 8 * i;

#ifdef _BLE_TRACE_
            Serial.print("U      : ");
        btct.printBytes(U,32);
        Serial.print("V      : ");
        btct.printBytes(V,32);
        Serial.print("X      : ");
        btct.printBytes(X,16);
        Serial.print("Y      : ");
        btct.printBytes(Y,16);
        Serial.print("g2res  : ");
        btct.printBytes(g2Result,4);
        Serial.print("Result : ");
        Serial.println(result);
#endif

            if (HCI._displayCode != 0) {
                HCI._displayCode(result % 1000000);
            }
            if (HCI._binaryConfirmPairing != 0) {
                if (!HCI._binaryConfirmPairing()) {
#ifdef _BLE_TRACE_
                    Serial.println("User rejection");
#endif
                    uint8_t rejection[2];
                    rejection[0] = CONNECTION_PAIRING_FAILED;
                    rejection[1] = 0x0C; // Numeric comparison failed
                    HCI.sendAclPkt(connectionHandle, SECURITY_CID, 2, rejection);
                    ATT.setPeerEncryption(connectionHandle, PEER_ENCRYPTION::NO_ENCRYPTION);
                } else {
#ifdef _BLE_TRACE_
                    Serial.println("User did confirm");
#endif
                }
            }
        }
        else {
            // PAIRING STAGE 6
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("*** STARTING STAGE 6 ***");
#endif
#ifdef _BLE_PAIRING_TRACE_
            Serial.println("Processing Incoming Random Request Response from peer");
#endif
#ifdef _BLE_PAIRING_TRACE_
            Serial.print("Getting Nb random value from peer device in L2CAP layer...");
#endif
            struct __attribute__ ((packed)) PairingRandom {
                uint8_t Nb[16];
            } *pairingRandom = (PairingRandom *) l2capSignalingHdr->data;
            for (int i = 0; i < 16; i++) {
                HCI.Nb[15 - i] = pairingRandom->Nb[i];
            }
#ifdef _BLE_PAIRING_TRACE_
            Serial.println"HCI.Nb value set as: ");
 btct.printBytes(HCI.Nb, sizeof(HCI.Nb))

  Serial.println("Stored Nb random value to peer device!");
#endif

            // We now have all needed for compare value
            uint8_t g2Result[4];
            uint8_t U[32];
            uint8_t V[32];

            for (int i = 0; i < 32; i++) {
                U[31 - i] = HCI.localPublicKeyBuffer[i];
                V[31 - i] = HCI.remotePublicKeyBuffer[i];
            }

            btct.g2(U, V, HCI.Na, HCI.Nb, g2Result);
            uint32_t result = 0;
            for (int i = 0; i < 4; i++) {
                result += g2Result[3 - i] << 8 * i;
            }

#ifdef _BLE_PAIRING_TRACE_
            Serial.print("U      : ");
        btct.printBytes(U,32);
        Serial.print("V      : ");
        btct.printBytes(V,32);
        Serial.print("X      : ");
        btct.printBytes(HCI.Na ,16);
        Serial.print("Y      : ");
        btct.printBytes(HCI.Nb ,16);
        Serial.print("g2res  : ");
        btct.printBytes(g2Result,4);
        Serial.print("Result : ");
        Serial.println(result);
#endif
            validatePeerConfirmValue(connectionHandle);
        }
    }
    else if (code == CONNECTION_PAIRING_RESPONSE) {
        handlePairingRequestResponseAsInitiator(connectionHandle, l2capSignalingHdr);
    }
    else if (code == CONNECTION_PAIRING_FAILED) {
#ifdef _BLE_TRACE_
        struct __attribute__ ((packed)) PairingFailed
        {
          uint8_t code;
          uint8_t reason;
        } *pairingFailed = (PairingFailed*)data;
        Serial.print("Pairing failed with code: 0x");
        Serial.println(pairingFailed->reason,HEX);
#endif
        ATT.setPeerEncryption(connectionHandle, PEER_ENCRYPTION::NO_ENCRYPTION);
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
        Serial.println("PAIRING FAILED --- Set Peer Encryption to NO_ENCRYPTION");
#endif
    }
    else if (code == CONNECTION_IDENTITY_INFORMATION) {
        struct __attribute__ ((packed)) IdentityInformation {
            uint8_t code;
            uint8_t PeerIRK[16];
        } *identityInformation = (IdentityInformation *) data;
        for (int i = 0; i < 16; i++) ATT.peerIRK[15 - i] = identityInformation->PeerIRK[i];
#ifdef _BLE_TRACE_
        Serial.println("Saved peer IRK");
#endif
    }
    else if (code == CONNECTION_IDENTITY_ADDRESS) {
        struct __attribute__ ((packed)) IdentityAddress {
            uint8_t code;
            uint8_t addressType;
            uint8_t address[6];
        } *identityAddress = (IdentityAddress *) data;
        // we can save this information now.
        uint8_t peerAddress[6];
        for (int i = 0; i < 6; i++) peerAddress[5 - i] = identityAddress->address[i];

        HCI.saveNewAddress(identityAddress->addressType, peerAddress, ATT.peerIRK, ATT.localIRK);
        if (HCI._storeLTK != 0) {
            HCI._storeLTK(peerAddress, HCI.LTK);
        }
    }
    else if (code == CONNECTION_PAIRING_PUBLIC_KEY) {
        /// Received a public key
        struct __attribute__ ((packed)) ConnectionPairingPublicKey {
            uint8_t x[32];
            uint8_t y[32];
        } *connectionPairingPublicKey = (ConnectionPairingPublicKey *) l2capSignalingHdr->data;
        struct __attribute__ ((packed)) GenerateDHKeyCommand {
            uint8_t x[32];
            uint8_t y[32];
        } generateDHKeyCommand = {
                0x00,
                0x00,
        };
        memcpy(generateDHKeyCommand.x, connectionPairingPublicKey->x, 32);
        memcpy(generateDHKeyCommand.y, connectionPairingPublicKey->y, 32);

        if (ATT.setPeerEncryption(connectionHandle,
                                  ATT.getPeerEncryption(connectionHandle) | PEER_ENCRYPTION::REQUESTED_ENCRYPTION)) {
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("Set Peer Encryption to REQUESTED_ENCRYPTION");
#endif
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("[Info] Pairing public key");
            Serial.println("Requested encryption stored.");
#endif
        } else {
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("[Info] Pairing public key");
            Serial.print("Failed to store encryption request with handle: 0x");
            Serial.println(connectionHandle,HEX);
#endif
        }

        //If the public key was sent to us first we want to send one back, we can check if the peer was the initiator of the pairing request to determine this as they would send first if so
        if (ATT.getPeerPairingInitiatorRelationship(connectionHandle))
        {
            memcpy(HCI.remotePublicKeyBuffer, &generateDHKeyCommand, sizeof(generateDHKeyCommand));
            HCI.sendCommand((OGF_LE_CTL << 10) | LE_COMMAND::READ_LOCAL_P256, 0);
        }
        else
        {
            // PAIRING STAGE 3
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("*** STARTING STAGE 3 ***");
#endif
            //Just store the peers public key in the remote buffer and then wait for confirm response
            memcpy(HCI.remotePublicKeyBuffer, &generateDHKeyCommand, sizeof(generateDHKeyCommand));

            HCI.sendCommand((OGF_LE_CTL << 10) | LE_COMMAND::GENERATE_DH_KEY_V1,
                            sizeof(HCI.remotePublicKeyBuffer), HCI.remotePublicKeyBuffer);

            HCI.poll();
        }


    }
    else if (code == CONNECTION_PAIRING_DHKEY_CHECK) {
        uint8_t RemoteDHKeyCheck[16];
        for (int i = 0; i < 16; i++) {
            RemoteDHKeyCheck[15 - i] = l2capSignalingHdr->data[i];
        }

        if (ATT.getPeerPairingInitiatorRelationship(connectionHandle))
        {

#ifdef _BLE_TRACE_
            Serial.println("[Info] DH Key check");
        Serial.print("Remote DHKey Check: ");
        btct.printBytes(RemoteDHKeyCheck, 16);
#endif


            uint8_t encryptionState = ATT.getPeerEncryption(connectionHandle) | PEER_ENCRYPTION::RECEIVED_DH_CHECK;
            ATT.setPeerEncryption(connectionHandle, encryptionState);
            if ((encryptionState & PEER_ENCRYPTION::DH_KEY_CALULATED) == 0) {
#ifdef _BLE_TRACE_
                Serial.println("DHKey not yet ready, will calculate f5, f6 later");
#endif
                // store RemoteDHKeyCheck for later check
                memcpy(HCI.remoteDHKeyCheckBuffer, RemoteDHKeyCheck, 16);

            } else {
                // We've already calculated the DHKey so we can calculate our check and send it.
                smCalculateLTKandConfirm(connectionHandle, RemoteDHKeyCheck);

            }
        }
        else
        {
            // PAIRING STAGE 9
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("*** STARTING STAGE 9 ***");
            Serial.println("DHK Check received from peer device. Value: ");
            btct.printBytes(RemoteDHKeyCheck, sizeof(RemoteDHKeyCheck));
#endif

            uint8_t encryptionState = ATT.getPeerEncryption(connectionHandle) | PEER_ENCRYPTION::RECEIVED_DH_CHECK;
            ATT.setPeerEncryption(connectionHandle, encryptionState);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.println("Set Peer Encryption to RECEIVED_DH_CHECK");
#endif

            uint8_t localAddress[7];
            uint8_t remoteAddress[7];
            ATT.getPeerAddrWithType(handle, remoteAddress);

            HCI.readBdAddr();
            memcpy(&localAddress[1], HCI.localAddr, 6);
            localAddress[0] = 0; // First byte of 0 indicates it's a public address

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.print("Local Address set as:");
btct.printBytes(localAddress, sizeof(localAddress));
    Serial.print("Remote Address set as:");
btct.printBytes(remoteAddress, sizeof(remoteAddress));
#endif


#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.print("Retrieved MAC Key as:");
btct.printBytes(HCI.MacKey, sizeof(HCI.MacKey));
    Serial.print("Retrieved LTK as:");
btct.printBytes(HCI.LTK, sizeof(HCI.LTK));
#endif

            // Compute Eb
            uint8_t Eb[16];
            uint8_t R[16];
            uint8_t SlaveIOCap[3];

            ATT.getPeerIOCap(handle, SlaveIOCap);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.print("Master IO Cap to use:");
btct.printBytes(MasterIOCap, sizeof(MasterIOCap));
#endif

            //Generate R as all 0's as per spec
            for (int i = 0; i < 16; i++) {
                R[i] = 0;
            }

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.print("R Value to use:");
btct.printBytes(R, sizeof(R));
   Serial.println("Calculating Eb value using f6 function");
#endif

            btct.f6(HCI.MacKey, HCI.Nb, HCI.Na, R, SlaveIOCap, remoteAddress, localAddress, Eb);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
            Serial.print("Eb value calulated as:");
btct.printBytes(Eb, sizeof(Eb));
#endif


            if (memcmp(Eb, RemoteDHKeyCheck, 16) == 0) {
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
                Serial.println("DHKey check successful. Received value matches our Eb value. Auth Stage 2 complete!);
#endif

            } else {
                // Check failed, abort pairing
                uint8_t ret[2] = {CONNECTION_PAIRING_FAILED, 0x0B}; // 0x0B = DHKey Check Failed
                HCI.sendAclPkt(handle, SECURITY_CID, sizeof(ret), ret);
                ATT.setPeerEncryption(handle, NO_ENCRYPTION);
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
                Serial.println("Error: DHKey check failed - Aborting");
                Serial.println("Set Peer Encryption to NO_ENCRYPTION");
#endif
            }


        }
    }
    else if (code == CONNECTION_PAIRING_CONFIRM)
    {
        // PAIRING STAGE 4
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
        Serial.println("*** STARTING STAGE 4 ***");
#endif
#ifdef _BLE_PAIRING_TRACE_
        Serial.print("Pairing Confirm response returned from handle: ");
    Serial.println(connectionHandle);
#endif
        struct __attribute__ ((packed)) PairingConfirm {
            uint8_t cb[16];
        } *pairingConfirm = (PairingConfirm *) l2capSignalingHdr->data;

#ifdef _BLE_PAIRING_TRACE_
        Serial.print("Confirm Value from peer: ");
        btct.printBytes(pairingConfirm->cb, 16);
#endif
        ATT.setPeerPairingConfirmValue(connectionHandle, pairingConfirm->cb);
        sendRandomValue(connectionHandle);
        HCI.poll();
    }
}

bool L2CAPSignalingClass::validatePeerConfirmValue(uint16_t connectionHandle)
{
    uint8_t Z = 0;
    struct __attribute__ ((packed)) F4Params {
        uint8_t U[32];
        uint8_t V[32];
        uint8_t Z;
    } f4Params;

    memset(&f4Params,0, sizeof(F4Params));

    for (int i = 0; i < 32; i++) {
        f4Params.U[31 - i] = HCI.localPublicKeyBuffer[i];
        f4Params.V[31 - i] = HCI.remotePublicKeyBuffer[i];
    }

    uint8_t Cb_local[16];
    btct.AES_CMAC(HCI.Nb, (unsigned char *) &f4Params, sizeof(f4Params), Cb_local);

    uint8_t newConfirmValue[16];
    for (int i = 0; i < 16; i++) {
        newConfirmValue[15 - i] = Cb_local[i];
    }

    uint8_t originalConfirmValue[16];
    ATT.getPeerPairingConfirmValue(connectionHandle, *originalConfirmValue);

    if (memcmp(newConfirmValue, originalConfirmValue, sizeof(originalConfirmValue)) == 0) {
     return true;

    } else {
        Serial.println("The confirm values do not match.");
        uint8_t ret[2] = {CONNECTION_PAIRING_FAILED, 0x04}; // 0x0B = DHKey Check Failed
        HCI.sendAclPkt(connectionHandle, SECURITY_CID, sizeof(ret), ret);
        ATT.setPeerEncryption(connectionHandle, NO_ENCRYPTION);
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
        Serial.println("Confirm value mismatch --- Set Peer Encryption to NO ECNRYPTION");
#endif
        return false;
    }
}

// PAIRING STAGE 2
void L2CAPSignalingClass::handlePairingRequestResponseAsInitiator(uint16_t connectionHandle, L2CAPSignalingHdr l2capSignalingHdr)
{
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("*** STARTING STAGE 2 ***");
#endif
#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Response to Send Pairing Request Received in L2CAPSignaling Layer from: ");
    Serial.println(handle);
#endif
        struct __attribute__ ((packed)) PairingResponse {
            uint8_t ioCapability;
            uint8_t oobDataFlag;
            uint8_t authReq;
            uint8_t maxEncSize;
            uint8_t initiatorKeyDistribution;
            uint8_t responderKeyDistribution;
        } *pairingResponse = (PairingResponse *) l2capSignalingHdr->data;

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Pairing Request Response from peer device has payload: ");
        btct.printBytes(data, sizeof(data));
#endif

        KeyDistribution responseKD = KeyDistribution();
        responseKD.setIdKey(true);

        //Set these the same as what the peer device will accept
        ATT.remoteKeyDistribution = responseKD; // KeyDistribution(pairingRequest->initiatorKeyDistribution);
        ATT.localKeyDistribution = responseKD; //KeyDistribution(pairingRequest->responderKeyDistribution);
        // KeyDistribution rkd(pairingRequest->responderKeyDistribution);
        AuthReq req(pairingResponse->authReq);

#ifdef _BLE_PAIRING_TRACE_
        Serial.print("Response has properties: ");
            Serial.print(req.Bonding()?"bonding, ":"no bonding, ");
            Serial.print(req.CT2()?"CT2, ":"no CT2, ");
            Serial.print(req.KeyPress()?"KeyPress, ":"no KeyPress, ");
            Serial.print(req.MITM()?"MITM, ":"no MITM, ");
            Serial.print(req.SC()?"SC, ":"no SC, ");
#endif

        uint8_t peerIOCap[3];
        peerIOCap[0] = pairingResponse->authReq;
        peerIOCap[1] = pairingResponse->oobDataFlag;
        peerIOCap[2] = pairingResponse->ioCapability;
        ATT.setPeerIOCap(connectionHandle, peerIOCap);

#ifdef _BLE_PAIRING_TRACE_
        Serial.print("Set Peer IO Cap as: ");
               btct.printBytes(peerIOCap, sizeof(peerIOCap));

               Serial.print("Peer encryption : 0b");
            Serial.println(ATT.getPeerEncryption(connectionHandle), BIN);

                          Serial.print("Requesting Public Key Generation from Controller");
#endif
//Generate a public key to send
    HCI.sendCommand((OGF_LE_CTL << 10) | LE_COMMAND::READ_LOCAL_P256, 0);

#ifdef _BLE_PAIRING_TRACE_
                          Serial.print("Public Key Generation requested!");
#endif

}

void L2CAPSignalingClass::handlePairingRequestFromInitiator(uint16_t connectionHandle, L2CAPSignalingHdr l2capSignalingHdr)
{
    if (isPairingEnabled()) {
        if (_pairing_enabled >= 2) _pairing_enabled = 0;  // 2 = pair once only

        // 0x1
        struct __attribute__ ((packed)) PairingRequest {
            uint8_t ioCapability;
            uint8_t oobDataFlag;
            uint8_t authReq;
            uint8_t maxEncSize;
            uint8_t initiatorKeyDistribution;
            uint8_t responderKeyDistribution;
        } *pairingRequest = (PairingRequest *) l2capSignalingHdr->data;

        KeyDistribution responseKD = KeyDistribution();
        responseKD.setIdKey(true);

        ATT.remoteKeyDistribution = responseKD;// KeyDistribution(pairingRequest->initiatorKeyDistribution);
        ATT.localKeyDistribution = responseKD; //KeyDistribution(pairingRequest->responderKeyDistribution);
        // KeyDistribution rkd(pairingRequest->responderKeyDistribution);
        AuthReq req(pairingRequest->authReq);
#ifdef _BLE_TRACE_
        Serial.print("Req has properties: ");
            Serial.print(req.Bonding()?"bonding, ":"no bonding, ");
            Serial.print(req.CT2()?"CT2, ":"no CT2, ");
            Serial.print(req.KeyPress()?"KeyPress, ":"no KeyPress, ");
            Serial.print(req.MITM()?"MITM, ":"no MITM, ");
            Serial.print(req.SC()?"SC, ":"no SC, ");
#endif

        uint8_t peerIOCap[3];
        peerIOCap[0] = pairingRequest->authReq;
        peerIOCap[1] = pairingRequest->oobDataFlag;
        peerIOCap[2] = pairingRequest->ioCapability;

        ATT.setPeerIOCap(connectionHandle, peerIOCap);

        ATT.setPeerEncryption(connectionHandle,
                              ATT.getPeerEncryption(connectionHandle) | PEER_ENCRYPTION::PAIRING_REQUEST);
#ifdef _BLE_TRACE_
        Serial.print("Peer encryption : 0b");
            Serial.println(ATT.getPeerEncryption(connectionHandle), BIN);
#endif
        struct __attribute__ ((packed)) PairingResponse {
            uint8_t code;
            uint8_t ioCapability;
            uint8_t oobDataFlag;
            uint8_t authReq;
            uint8_t maxEncSize;
            uint8_t initiatorKeyDistribution;
            uint8_t responderKeyDistribution;
        } response = {CONNECTION_PAIRING_RESPONSE, HCI.localIOCap(), 0, HCI.localAuthreq().getOctet(), 0x10,
                      responseKD.getOctet(), responseKD.getOctet()};

        HCI.sendAclPkt(connectionHandle, SECURITY_CID, sizeof(response), &response);

    } else {
        // Pairing not enabled
        uint8_t ret[2] = {CONNECTION_PAIRING_FAILED, 0x05}; // reqect pairing
        HCI.sendAclPkt(connectionHandle, SECURITY_CID, sizeof(ret), ret);
        ATT.setPeerEncryption(connectionHandle, NO_ENCRYPTION);
    }
}

void L2CAPSignalingClass::smCalculateLTKandConfirm(uint16_t handle,
                                                   uint8_t expectedEa[]) { // Authentication stage 2: LTK Calculation

    uint8_t localAddress[7];
    uint8_t remoteAddress[7];
    ATT.getPeerAddrWithType(handle, remoteAddress);

    HCI.readBdAddr();
    memcpy(&localAddress[1], HCI.localAddr, 6);
    localAddress[0] = 0; // IOT 33 uses a static address // TODO: confirm for Nano BLE

    // Compute the LTK and MacKey
    uint8_t MacKey[16];
    btct.f5(HCI.DHKey, HCI.Na, HCI.Nb, remoteAddress, localAddress, MacKey, HCI.LTK);

    // Compute Ea and Eb
    uint8_t Ea[16];
    uint8_t Eb[16];
    uint8_t R[16];
    uint8_t MasterIOCap[3];
    uint8_t SlaveIOCap[3] = {HCI.localAuthreq().getOctet(), 0x0, HCI.localIOCap()};

    ATT.getPeerIOCap(handle, MasterIOCap);
    for (int i = 0; i < 16; i++) R[i] = 0;

    btct.f6(MacKey, HCI.Na, HCI.Nb, R, MasterIOCap, remoteAddress, localAddress, Ea);
    btct.f6(MacKey, HCI.Nb, HCI.Na, R, SlaveIOCap, localAddress, remoteAddress, Eb);

#ifdef _BLE_TRACE_
    Serial.println("Calculate and confirm LTK via f5, f6:");
    Serial.print("DHKey      : ");  btct.printBytes(HCI.DHKey,32);
    Serial.print("Na         : ");  btct.printBytes(HCI.Na,16);
    Serial.print("Nb         : ");  btct.printBytes(HCI.Nb,16);
    Serial.print("MacKey     : ");  btct.printBytes(MacKey,16);
    Serial.print("LTK        : ");  btct.printBytes(HCI.LTK,16);
    Serial.print("Expected Ea: ");  btct.printBytes(expectedEa, 16);
    Serial.print("Ea         : ");  btct.printBytes(Ea, 16);
    Serial.print("Eb         : ");  btct.printBytes(Eb,16);
    Serial.print("Local Addr : ");  btct.printBytes(localAddress, 7);
    Serial.print("LocalIOCap : ");  btct.printBytes(SlaveIOCap, 3);
    Serial.print("MasterAddr : ");  btct.printBytes(remoteAddress, 7);
    Serial.print("MasterIOCAP: ");  btct.printBytes(MasterIOCap, 3);
#endif

    // Check if Ea = expectedEa
    if (memcmp(Ea, expectedEa, 16) == 0) {
        // Check ok
        // Send our confirmation value to complete authentication stage 2
        uint8_t ret[17];
        ret[0] = CONNECTION_PAIRING_DHKEY_CHECK;
        for (int i = 0; i < sizeof(Eb); i++) {
            ret[sizeof(Eb) - i] = Eb[i];
        }
        HCI.sendAclPkt(handle, SECURITY_CID, sizeof(ret), ret);
        uint8_t encryption = ATT.getPeerEncryption(handle) | PEER_ENCRYPTION::SENT_DH_CHECK;
        ATT.setPeerEncryption(handle, encryption);
#ifdef _BLE_TRACE_
        Serial.println("DHKey check ok - send Eb back");
#endif

    } else {
        // Check failed, abort pairing
        uint8_t ret[2] = {CONNECTION_PAIRING_FAILED, 0x0B}; // 0x0B = DHKey Check Failed
        HCI.sendAclPkt(handle, SECURITY_CID, sizeof(ret), ret);
        ATT.setPeerEncryption(handle, NO_ENCRYPTION);
#ifdef _BLE_TRACE_
        Serial.println("Error: DHKey check failed - Aborting");
#endif
    }
}

void L2CAPSignalingClass::removeConnection(uint8_t /*handle*/, uint16_t /*reason*/) {
}

void L2CAPSignalingClass::setConnectionInterval(uint16_t minInterval, uint16_t maxInterval) {
    _minInterval = minInterval;
    _maxInterval = maxInterval;
}

void L2CAPSignalingClass::setSupervisionTimeout(uint16_t supervisionTimeout) {
    _supervisionTimeout = supervisionTimeout;
}

void L2CAPSignalingClass::setPairingEnabled(uint8_t enabled) {
    _pairing_enabled = enabled;
}

bool L2CAPSignalingClass::isPairingEnabled() {
    return _pairing_enabled > 0;
}

void L2CAPSignalingClass::connectionParameterUpdateRequest(uint16_t handle, uint8_t identifier, uint8_t dlen,
                                                           uint8_t data[]) {
    struct __attribute__ ((packed)) L2CAPConnectionParameterUpdateRequest {
        uint16_t minInterval;
        uint16_t maxInterval;
        uint16_t latency;
        uint16_t supervisionTimeout;
    } *request = (L2CAPConnectionParameterUpdateRequest *) data;

    if (dlen < sizeof(L2CAPConnectionParameterUpdateRequest)) {
        // too short, ignore
        return;
    }

    struct __attribute__ ((packed)) L2CAPConnectionParameterUpdateResponse {
        uint8_t code;
        uint8_t identifier;
        uint16_t length;
        uint16_t value;
    } response = {CONNECTION_PARAMETER_UPDATE_RESPONSE, identifier, 2, 0x0000};

    if (_minInterval && _maxInterval) {
        if (request->minInterval < _minInterval || request->maxInterval > _maxInterval) {
            response.value = 0x0001; // reject
        }
    }

    if (_supervisionTimeout) {
        if (request->supervisionTimeout != _supervisionTimeout) {
            response.value = 0x0001; // reject
        }
    }

    HCI.sendAclPkt(handle, SIGNALING_CID, sizeof(response), &response);

    if (response.value == 0x0000) {
        HCI.leConnUpdate(handle, request->minInterval, request->maxInterval, request->latency,
                         request->supervisionTimeout);
    }
}

void
L2CAPSignalingClass::connectionParameterUpdateResponse(uint16_t /*handle*/, uint8_t /*identifier*/, uint8_t /*dlen*/,
                                                       uint8_t /*data*/[]) {
}

// PAIRING STAGE 1C
bool L2CAPSignalingClass::initiatePairingRequest(uint16_t handle) {
#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Creating Pairing Request payload struct in L2CAP layer...");
    Serial.println(handle);
#endif
    struct __attribute__ ((packed)) PairingRequest {
        uint8_t opcode;
        uint8_t ioCapability;
        uint8_t oobDataFlag;
        uint8_t authReq;
        uint8_t maxEncSize;
        uint8_t initiatorKeyDistribution;
        uint8_t responderKeyDistribution;
    };

// Define and initialize the PairingRequest object

    uint8_t localIOCap[3];
    peerIOCap[0] = 0x9; //Auth
    peerIOCap[1] = 0x00; //OOB
    peerIOCap[2] = IOCAP_DISPLAY_ONLY; //IO

    ATT.setLocalIOCap(localIOCap);

    PairingRequest pr{};
    pr.opcode = CONNECTION_PAIRING_REQUEST; //OPCode signalling we are sending a pairing request
    pr.ioCapability = peerIOCap[2]; //TODO: Only implementing JustWorks pairing at this time
    pr.oobDataFlag = peerIOCap[1]; //No Out of Bounds Capability TODO: Set from authReq object
    pr.authReq = peerIOCap[0]; //Set auth Capabilities
    pr.maxEncSize = 0x10;
    pr.initiatorKeyDistribution = 0x01;
    pr.responderKeyDistribution = 0x01;

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Creating Pairing Request payload struct in L2CAP layer...");
    Serial.println(handle);
#endif

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Sending Pairing Request ACL Packet...");
    Serial.println(handle);
    Serial.print("ACL Packet Data:");
    btct.printBytes(pr, sizeof(pr));
#endif

    HCI.sendAclPkt(handle, SECURITY_CID, sizeof(pr), &pr);

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Pairing Request ACL Packet Sent!");
    Serial.println(handle);
#endif

    ATT.setPeerEncryption(connectionHandle,
                          ATT.getPeerEncryption(handle) | PEER_ENCRYPTION::PAIRING_REQUEST);

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Peer encryption : 0b");
            Serial.println(ATT.getPeerEncryption(connectionHandle), BIN);
#endif
    return true;
}

// PAIRING STAGE 5
void L2CAPSignalingClass::sendRandomValue(uint16_t handle) {
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("*** STARTING STAGE 5 ***");
#endif
#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Generating Na random value for this device as initiator in L2CAP layer...");
    Serial.println(handle);
#endif

    uint8_t Na[16];
    HCI.leRand(Na);
    HCI.leRand(&Na[8]);

    for (int i = 0; i < 16; i++) {
        HCI.Na[15 - i] = Na[i];
    }

#ifdef _BLE_PAIRING_TRACE_
    Serial.print("Sending Na random value to peer device in L2CAP layer...");
#endif

    struct __attribute__ ((packed)) PairingRequest {
        uint8_t code;
        uint8_t Na[16];
    } request = {CONNECTION_PAIRING_RANDOM, 0};
    memcpy(request.Na, HCI.Na, sizeof(HCI.Na));

    HCI.sendAclPkt(handle, SECURITY_CID, sizeof(request), &request);

#ifdef _BLE_PAIRING_TRACE_
    Serial.println"HCI.Na value set as: ");
 btct.printBytes(HCI.Na, sizeof(HCI.Na))

  Serial.println("Sent Na random value to peer device!");
#endif

}
// PAIRING STAGE 8
void L2CAPSignalingClass::sendDHKCheck(uint8_t handle) {
#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("*** STARTING STAGE 8 ***");
    Serial.println("Sending DHK Check to Peet in L2CAP Layer");
    Serial.println("Retrieve local and remote BD addresses");
#endif

    uint8_t localAddress[7];
    uint8_t remoteAddress[7];
    ATT.getPeerAddrWithType(handle, remoteAddress);

    HCI.readBdAddr();
    memcpy(&localAddress[1], HCI.localAddr, 6);
    localAddress[0] = 0; // First byte of 0 indicates it's a public address

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.print("Local Address set as:");
btct.printBytes(localAddress, sizeof(localAddress));
    Serial.print("Remote Address set as:");
btct.printBytes(remoteAddress, sizeof(remoteAddress));
#endif

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("Calculating MacKey and LTK using f5");
#endif

    btct.f5(HCI.DHKey, HCI.Na, HCI.Nb, localAddress, remoteAddress, HCI.MacKey, HCI.LTK);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.print("MAC Key generated as:");
btct.printBytes(HCI.MacKey, sizeof(MacKey));
    Serial.print("LTK generated as:");
btct.printBytes(HCI.LTK, sizeof(HCI.LTK));
#endif

    // Compute Ea and Eb
    uint8_t Ea[16];
    uint8_t R[16];
    uint8_t MasterIOCap[3];

    ATT.getLocalIOCap(MasterIOCap);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.print("Master IO Cap to use:");
btct.printBytes(MasterIOCap, sizeof(MasterIOCap));
#endif

    //Generate R as all 0's as per spec
    for (int i = 0; i < 16; i++) {
        R[i] = 0;
    }

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.print("R Value to use:");
btct.printBytes(R, sizeof(R));
   Serial.println("Calculating Ea value using f6 function");
#endif

    btct.f6(HCI.MacKey, HCI.Na, HCI.Nb, R, MasterIOCap, localAddress, remoteAddress,  Ea);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.print("Ea value calulated as:");
btct.printBytes(Ea, sizeof(Ea));
#endif

    uint8_t ret[17];
    ret[0] = CONNECTION_PAIRING_DHKEY_CHECK;
    for (int i = 0; i < sizeof(Ea); i++) {
        ret[sizeof(Ea) - i] = Ea[i];
    }

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("Sending DHK Check request to peer...");
#endif

    HCI.sendAclPkt(handle, SECURITY_CID, sizeof(ret), ret);
    uint8_t encryption = ATT.getPeerEncryption(handle) | PEER_ENCRYPTION::SENT_DH_CHECK;
    ATT.setPeerEncryption(handle, encryption);

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("DHK Check request sent to peer.");
#endif

#if defined(_BLE_TRACE_) || defined(_BLE_PAIRING_TRACE_)
    Serial.println("Set Peer Encryption to SENT_DH_CHECK");
#endif

    HCI.poll();
}

#if !defined(FAKE_L2CAP)
L2CAPSignalingClass L2CAPSignalingObj;
L2CAPSignalingClass &L2CAPSignaling = L2CAPSignalingObj;
#endif
