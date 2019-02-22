#pragma once
#include "bytes.hh"
#include "seastar/core/future.hh"
#include "seastar/core/sstring.hh"
#include "redis/request.hh"
#include "redis/reply.hh"
#include "mutation.hh"
#include "db/consistency_level_type.hh"
#include "db/timeout_clock.hh"
#include "db/system_keyspace.hh"
#include "service/storage_proxy.hh"
#include "keys.hh"
#include "timestamp.hh"
using namespace seastar;

class timeout_config;

namespace service {
//class storage_proxy;
class client_state;
}

namespace cql3 {
class query_options;
}

namespace tracing {
class trace_state_ptr;
}

namespace redis {

static inline decltype(auto) keyspace() { return db::system_keyspace::redis::NAME; }
static inline decltype(auto) simple_objects() { return db::system_keyspace::redis::SIMPLE_OBJECTS; }
static inline decltype(auto) lists() { return db::system_keyspace::redis::LISTS; }
static inline decltype(auto) sets() { return db::system_keyspace::redis::SETS; }
static inline decltype(auto) maps() { return db::system_keyspace::redis::MAPS; }
static inline const schema_ptr simple_objects_schema(service::storage_proxy& proxy) {
    auto& db = proxy.get_db().local();
    auto schema = db.find_schema(keyspace(), simple_objects());
    return schema;
}
static inline const schema_ptr lists_schema(service::storage_proxy& proxy) {
    auto& db = proxy.get_db().local();
    auto schema = db.find_schema(keyspace(), lists());
    return schema;
}
static inline const schema_ptr sets_schema(service::storage_proxy& proxy) {
    auto& db = proxy.get_db().local();
    auto schema = db.find_schema(keyspace(), sets());
    return schema;
}
static inline const schema_ptr maps_schema(service::storage_proxy& proxy) {
    auto& db = proxy.get_db().local();
    auto schema = db.find_schema(keyspace(), maps());
    return schema;
}

struct partition_dead_tag {}; 
class abstract_command : public enable_shared_from_this<abstract_command> {
protected:
    bytes _name;
    // ttl in millis seconds
    gc_clock::duration _ttl;
    const api::timestamp_type _timestamp;
    const gc_clock::time_point _local_deletion_time;
public:
    abstract_command(bytes&& name, const gc_clock::duration ttl)
        : _name(std::move(name))
        , _ttl(ttl)
        , _timestamp(api::new_timestamp())
        , _local_deletion_time(gc_clock::now())
    {
    }
    abstract_command(bytes&& name)
        : _name(std::move(name))
        , _timestamp(api::new_timestamp())
        , _local_deletion_time(gc_clock::now())
    {
    }
    virtual ~abstract_command() {};
    
    virtual future<reply> execute(service::storage_proxy&, db::consistency_level cl, db::timeout_clock::time_point, const timeout_config& tc, service::client_state& client_state) = 0;
    const bytes& name() const { return _name; }
    static inline sstring make_sstring(const bytes& b) {
        return sstring{reinterpret_cast<const char*>(b.data()), b.size()};
    }
    /*
    static inline gc_clock::duration make_ttl(long ttl) {
        return db_clock::from_time_t({ 0 }) + std::chrono::milliseconds(ttl * 1000);
    }
    */
    inline long bytes2long(const bytes& b) {
        try {
            return std::atol(make_sstring(b).data());
        } catch (std::exception const & e) {
            throw e;
        }
    }
    inline bytes long2bytes(long l) {
        auto s = sprint("%lld", l);
        return to_bytes(s);
    }
    inline bool is_number(const bytes& b)
    {
        return !b.empty() && std::find_if(b.begin(), b.end(), [] (auto c) { return !std::isdigit((char)c); }) == b.end();
    }

    atomic_cell make_dead_cell() const {
        return atomic_cell::make_dead(_timestamp, _local_deletion_time);
    }   

    atomic_cell make_cell(const schema_ptr schema, const abstract_type& type, const fragmented_temporary_buffer::view& value, atomic_cell::collection_member cm = atomic_cell::collection_member::no) const {
        auto ttl = _ttl;

        if (ttl.count() <= 0) {
            ttl = schema->default_time_to_live();
        }   

        if (ttl.count() > 0) {
            return atomic_cell::make_live(type, _timestamp, value, _local_deletion_time + ttl, ttl, cm);
        } else {
            return atomic_cell::make_live(type, _timestamp, value, cm);
        }   
    };  

    atomic_cell make_cell(const schema_ptr schema, const abstract_type& type, bytes_view value, atomic_cell::collection_member cm = atomic_cell::collection_member::no) const {
        return make_cell(schema, type, fragmented_temporary_buffer::view(value), cm);
    }   
   
    mutation make_mutation(const schema_ptr schema, const bytes& key) {
        auto pkey = partition_key::from_single_value(*schema, utf8_type->decompose(abstract_command::make_sstring(key)));
        return std::move(mutation(schema, std::move(pkey)));
    }
    future<> write_mutation(service::storage_proxy&, const schema_ptr schema, const bytes& key, bytes&& data, db::consistency_level cl, db::timeout_clock::time_point timeout, service::client_state& client_state);
    future<> write_mutation(service::storage_proxy&, const schema_ptr schema, const bytes& key, partition_dead_tag, db::consistency_level cl, db::timeout_clock::time_point timeout, service::client_state& client_state);
};

// Read required partition for write-before-read operations.
struct prefetched_partition_collection {
    const schema_ptr _schema;
    bool _inited = false;
    struct cell {
        bytes _key;
        bytes _value;
    };
    using cell_list = std::vector<cell>;
    using row = cell_list; //std::unordered_map<bytes, cell_list>;
    row _row;
    prefetched_partition_collection(const schema_ptr schema) : _schema(schema) {}
    row& partition() { return _row; }
};

struct prefetched_partition_simple {
    const schema_ptr _schema;
    bytes _data;
    bool _inited;
    prefetched_partition_simple(const schema_ptr schema, bytes&& b) : _schema(schema), _data(std::move(b)), _inited(true) {}
    prefetched_partition_simple(const schema_ptr schema) : _schema(schema), _inited(false) {}
    const bool& fetched() const { return _inited; }
};

class prefetch_partition_helper final {
public:
    static future<std::unique_ptr<prefetched_partition_simple>> prefetch_simple(service::storage_proxy& proxy,
        const schema_ptr schema,
        const bytes& raw_key,
        db::consistency_level cl,
        db::timeout_clock::time_point timeout,
        service::client_state& cs);
    static future<std::unique_ptr<prefetched_partition_collection>> prefetch_collection(service::storage_proxy& proxy,
        const schema_ptr schema,
        const bytes& raw_key,
        db::consistency_level cl,
        db::timeout_clock::time_point timeout,
        service::client_state& cs);
    static future<bool> exists(service::storage_proxy& proxy,
        const schema_ptr schema,
        const bytes& raw_key,
        db::consistency_level cl,
        db::timeout_clock::time_point timeout,
        service::client_state& cs);
};
} // end of redis namespace