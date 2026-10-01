#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <packet.h>

#define SSID "Voyager"
#define PASSWORD "7neHHvcuz3eMSYUE"
#define PORT 25565

WiFiServer tcpServer(PORT);

#define MAX_TCP_CONNECTIONS 4
WiFiClient clients[MAX_TCP_CONNECTIONS];

#define BUFFER_SIZE 256
char tcp_buffer[MAX_TCP_CONNECTIONS][BUFFER_SIZE];

game::Player players[MAX_TCP_CONNECTIONS];


/** AP 模式：启动后打印一次状态 */
void check_ap_connection()
{
    static bool printed = false;
    if (!printed) {
        Serial.print("AP ready, IP: ");
        Serial.println(WiFi.softAPIP());
        printed = true;
    }
}


void handle_new_connections()
{
    WiFiClient client = tcpServer.accept();
    if (client)
    {
        Serial.print(F("New connection from "));
        Serial.println(client.remoteIP().toString());

        for (int i = 0; i < MAX_TCP_CONNECTIONS; i++)
        {
            if (!clients[i].connected())
            {
                clients[i] = client;
                players[i].state = 0;
                players[i].eid = i;
                players[i].lastKeepalive = millis();
                players[i].unloadAllChunks();
                tcp_buffer[i][0]='\0';
                Serial.print(F("Kanal="));
                Serial.println(i);
                return;
            }
        }
        Serial.println(F("To many connections"));
        client.stop();
    }
}

void process_incoming_tcp()
{
    static int i=0;

    // 清理断开的连接
    if (players[i].state != 0 && !clients[i].connected()) {
        Serial.print("Client ");
        Serial.print(i);
        Serial.println(" disconnected, cleaning up");
        clients[i].stop();
        players[i].state = 0;
        players[i].unloadAllChunks();
        players[i].lastChunkTime = 0;
    }

    if (clients[i].available())
    {
        auto client = clients[i];
        int packetLength = packet::util::readVarInt(client);
        int packetType = packet::util::readVarInt(client);

        // Handshake
        if (packetType == 0 && players[i].state == 0) {
            packet::Handshake handshake(client);
            Serial.print("Protocol Version: ");
            Serial.println(handshake.protocolVersion);
            Serial.print("Next State: ");
            Serial.println(handshake.nextState);
            players[i].state = handshake.nextState;
        }
        // Status
        else if (packetType == 0x00 && players[i].state == 1) {
            String json = "{\"version\":{\"name\":\"1.16.1\",\"protocol\":736},"
                          "\"players\":{\"max\":4,\"online\":0},"
                          "\"description\":{\"text\":\"ESP8266 MC Server\"}}";
            int jsonLen = json.length();
            int size = 1 + packet::util::varIntSize(jsonLen) + jsonLen;
            packet::util::writeVarInt(client, size);
            client.write(0x00);
            packet::util::writeVarInt(client, jsonLen);
            client.write(json.begin(), jsonLen);
            client.flush();
        }
        else if (packetType == 0x01 && players[i].state == 1) {
            uint8_t payload[8];
            for (int k = 0; k < 8; k++) payload[k] = client.read();
            client.write(9);
            client.write(0x01);
            client.write(payload, 8);
            client.flush();
            client.stop();
        }
        // Login Start
        else if (packetType == 0 && players[i].state == 2) {
            String playerName = packet::rLoginStart(client);
            Serial.print("Login Player: ");
            Serial.println(playerName);

            players[i].name = playerName;
            packet::util::uuid uuid { 42, 42, 42, 42, 42, 42, 42, 42 };
            String worldIdentifier = "minecraft:overworld";

            packet::wLoginSuccess(client, playerName, uuid);
            client.flush();
            packet::wJoinGame(client, 42, worldIdentifier, worldIdentifier, worldIdentifier, 2);
            client.flush();
            packet::wPosition(client, 3 * 16, 64, 3 * 16, 0, 0, 0x42, 0x00);
            client.flush();

            players[i].state = 3;
        }
        // Player Spawned
        else if (packetType == 0 && players[i].state == 3) {
            Serial.print("Client Spawned at tpid ");
            Serial.println(packet::util::readVarInt(client), HEX);
            players[i].state = 4;

            packet::wPlayerInfoAddPlayer(clients[i], players, MAX_TCP_CONNECTIONS);
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wPlayerInfoAddPlayer(clients[pid], { players[i] });
                }
                clients[pid].flush();
            }

            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wSpawnPlayer(clients[i], players[pid]);
                }
            }
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wSpawnPlayer(clients[pid], players[i]);
                }
            }

            packet::wChatMessage(client);
        }
        // Serverbound 1.16.1
        else if (packetType == 0x12 && players[i].state == 4) {
            game::Player p;
            packet::rPlayerPosition(client, p);
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wEntityPosition(clients[pid], players[i], p);
                }
            }
            players[i].posX = p.posX;
            players[i].posY = p.posY;
            players[i].posZ = p.posZ;
        }
        else if (packetType == 0x13 && players[i].state == 4) {
            game::Player p;
            packet::rPlayerPositionAndRotation(client, p);
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wEntityPositionRotation(clients[pid], players[i], p);
                }
            }
            players[i].posX = p.posX;
            players[i].posY = p.posY;
            players[i].posZ = p.posZ;
            players[i].yaw = p.yaw;
            players[i].pitch = p.pitch;
        }
        else if (packetType == 0x14 && players[i].state == 4) {
            packet::rPlayerRotation(client, players[i]);
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wEntityRotation(clients[pid], players[i]);
                }
            }
        }
        else if (packetType == 0x15 && players[i].state == 4) {
            packet::rPlayerMovement(client);
            for (byte pid = 0; pid < MAX_TCP_CONNECTIONS; ++pid) {
                if (pid != i && clients[pid].connected()) {
                    packet::wEntityMovement(clients[pid], players[i]);
                }
            }
        }
        else {
            Serial.print("Received Packet of type 0x");
            Serial.print(packetType, HEX);
            Serial.print(" len ");
            Serial.println(packetLength);
            for (int k = 0; k < packetLength - 1; k++) client.read();
        }
    }

    if (clients[i].connected() && players[i].state == 4) {
        if (millis() - players[i].lastKeepalive > 10000) {
            Serial.println("Sending keepalive...");
            players[i].lastKeepalive = millis();
            packet::wKeepAlive(clients[i]);
            clients[i].flush();
        }
        // 限速：每个玩家每 200ms 最多发一个区块
    if (millis() - players[i].lastChunkTime > 200) {
    game::Coordinate coordinate = players[i].nextChunk();
    if (coordinate.x != 255) {
        players[i].lastChunkTime = millis();
        Serial.print("Sending Chunk (");
        Serial.print(coordinate.x, DEC);
        Serial.print(", ");
        Serial.print(coordinate.y, DEC);
        Serial.println(")...");
        players[i].setChunkLoaded(coordinate.x, coordinate.y);
        packet::wChunk(clients[i], coordinate.x, coordinate.y);
        clients[i].flush();
        yield();
    }
}
    }

    if (++i >= MAX_TCP_CONNECTIONS)
    {
        i=0;
    }
}


void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== ESP8266 MC Server (AP mode, 1.16.1) ===");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(SSID, PASSWORD);

    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("AP SSID: ");
    Serial.print(SSID);
    Serial.print(" / PASS: ");
    Serial.println(PASSWORD);

    tcpServer.begin();
    Serial.println("TCP server listening on port 25565");
}


void loop()
{
    handle_new_connections();
    process_incoming_tcp();
    check_ap_connection();
}
