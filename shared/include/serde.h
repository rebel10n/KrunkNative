#pragma once
#include <stdlib.h>

#define PACKET_FIELD(type, name) type name;
#define PACKET_SIZE(type, name) sizeof_##type(packet->name) +
#define PACKET_SERIALIZE_FIELD(type, name) serialize_##type(packet->name, *buffer + offset); offset += sizeof_##type(packet->name);
#define PACKET_DESERIALIZE_FIELD(type, name) packet->name = deserialize_##type(buffer + offset); offset += sizeof_##type(packet->name);

#define DEFINE_PACKET(name) \
    typedef struct { \
        PACKET_FIELDS(PACKET_FIELD) \
    } packet_##name; \
    \
    inline size_t serialize_##name(packet_##name *packet, unsigned char **buffer) { \
        const size_t packet_size = PACKET_FIELDS(PACKET_SIZE) 0; \
        if (!(*buffer = malloc(packet_size))) return 0; \
        \
        size_t offset = 0; \
        PACKET_FIELDS(PACKET_SERIALIZE_FIELD) \
        \
        return packet_size; \
    } \
    \
    inline packet_##name deserialize_##name(unsigned char *buffer, size_t buffer_size) { \
        packet_##name p = {}; \
        packet_##name *packet = &p; \
        \
        const size_t packet_size = PACKET_FIELDS(PACKET_SIZE) 0; \
        if (buffer_size < packet_size) return p; \
        \
        size_t offset = 0; \
        PACKET_FIELDS(PACKET_DESERIALIZE_FIELD) \
        \
        return p; \
    }
