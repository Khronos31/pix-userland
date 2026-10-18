// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>

#include <dtv/userland/error.h>

#include <limits>

namespace dtv::userland::wire {
namespace {

class Writer final {
public:
    explicit Writer(MutableByteView output) noexcept : output_(output) {}

    bool ch(char value) noexcept
    {
        if (size_ == std::numeric_limits<std::size_t>::max()) return false;
        if (output_.data != nullptr && size_ < output_.size) output_.data[size_] = static_cast<std::uint8_t>(value);
        ++size_;
        return true;
    }

    bool text(const char* value) noexcept
    {
        while (*value != '\0') if (!ch(*value++)) return false;
        return true;
    }

    bool string(const char* value, std::size_t length) noexcept
    {
        static constexpr char hex[] = "0123456789abcdef";
        if (!ch('"')) return false;
        for (std::size_t i = 0U; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == '"' || c == '\\') {
                if (!ch('\\') || !ch(static_cast<char>(c))) return false;
            } else if (c == '\b') {
                if (!text("\\b")) return false;
            } else if (c == '\f') {
                if (!text("\\f")) return false;
            } else if (c == '\n') {
                if (!text("\\n")) return false;
            } else if (c == '\r') {
                if (!text("\\r")) return false;
            } else if (c == '\t') {
                if (!text("\\t")) return false;
            } else if (c < 0x20U) {
                if (!text("\\u00") || !ch(hex[c >> 4U]) || !ch(hex[c & 0x0fU])) return false;
            } else if (!ch(static_cast<char>(c))) return false;
        }
        return ch('"');
    }

    bool number(std::uint64_t value) noexcept
    {
        char digits[20];
        std::size_t count = 0U;
        do { digits[count++] = static_cast<char>('0' + value % 10U); value /= 10U; } while (value != 0U);
        while (count != 0U) if (!ch(digits[--count])) return false;
        return true;
    }

    bool signed_number(std::int32_t value) noexcept
    {
        if (value < 0) {
            if (!ch('-')) return false;
            return number(static_cast<std::uint64_t>(-(static_cast<std::int64_t>(value))));
        }
        return number(static_cast<std::uint64_t>(value));
    }

    bool quoted_number(std::uint64_t value) noexcept { return ch('"') && number(value) && ch('"'); }
    std::size_t size() const noexcept { return size_; }

private:
    MutableByteView output_;
    std::size_t size_ = 0U;
};

template <std::size_t N>
bool str(Writer& w, const WString<N>& value) noexcept { return w.string(value.bytes.data(), value.length); }

std::size_t length(const char* value) noexcept { std::size_t size=0U; while(value[size]!='\0') ++size; return size; }
bool key(Writer& w, const char* name) noexcept { return w.string(name, length(name)) && w.ch(':'); }
bool boolean(Writer& w, bool value) noexcept { return w.text(value ? "true" : "false"); }
bool nullable_u64(Writer& w, const Optional<std::uint64_t>& value) noexcept
{ return value.present ? w.quoted_number(value.value) : w.text("null"); }

bool producer(Writer& w, const Producer& v) noexcept
{
    return w.ch('{') && key(w,"product") && str(w,v.product) && w.ch(',') && key(w,"version") && str(w,v.version) &&
        w.ch(',') && key(w,"core_version") && (v.core_version.present ? str(w,v.core_version.value) : w.text("null")) && w.ch('}');
}
bool model(Writer& w, const Optional<Model>& v) noexcept
{
    if (!v.present) return w.text("null");
    return w.ch('{') && key(w,"id") && str(w,v.value.id) && w.ch(',') && key(w,"name") && str(w,v.value.name) && w.ch('}');
}
const char* identity_state(IdentitySerialState v) noexcept { return v==IdentitySerialState::present?"present":v==IdentitySerialState::absent?"absent":"unknown"; }
const char* uniqueness(Uniqueness v) noexcept { return v==Uniqueness::not_applicable?"not_applicable":v==Uniqueness::unique_in_scan?"unique_in_scan":v==Uniqueness::duplicate?"duplicate":"unknown"; }
bool identity(Writer& w,const Identity& v) noexcept
{
    return w.ch('{')&&key(w,"serial")&&(v.serial.present?str(w,v.serial.value):w.text("null"))&&w.ch(',')&&key(w,"serial_state")&&w.string(identity_state(v.serial_state),length(identity_state(v.serial_state)))&&w.ch(',')&&key(w,"uniqueness")&&w.string(uniqueness(v.uniqueness),length(uniqueness(v.uniqueness)))&&w.ch('}');
}
bool limits(Writer& w,const Limits& v) noexcept
{ return w.ch('{')&&key(w,"max_concurrent_streams")&&(v.max_concurrent_streams.present?w.number(v.max_concurrent_streams.value):w.text("null"))&&w.ch('}'); }
const char* topology_state(TopologyState v) noexcept { return v==TopologyState::complete?"complete":v==TopologyState::partial?"partial":"unknown"; }
bool topology(Writer& w,const Topology& v) noexcept
{ const char* s=topology_state(v.state); return w.ch('{')&&key(w,"state")&&w.string(s,length(s))&&w.ch(',')&&key(w,"expected_function_count")&&(v.expected_function_count.present?w.number(v.expected_function_count.value):w.text("null"))&&w.ch('}'); }
bool enclosure(Writer& w,const Optional<Enclosure>& v) noexcept
{
    if(!v.present)return w.text("null");
    return w.ch('{')&&key(w,"id")&&w.quoted_number(v.value.id)&&w.ch(',')&&key(w,"model")&&model(w,v.value.model)&&w.ch(',')&&key(w,"identity")&&identity(w,v.value.identity)&&w.ch(',')&&key(w,"limits")&&limits(w,v.value.limits)&&w.ch('}');
}
const char* mode(Mode v) noexcept{return v==Mode::cold?"cold":v==Mode::runtime?"runtime":"unknown";}
const char* serial_state(SerialState v) noexcept{return v==SerialState::present?"present":v==SerialState::absent?"absent":v==SerialState::unreadable?"unreadable":v==SerialState::invalid?"invalid":"not_queried";}
const char* identification_reason(IdentificationReason v) noexcept{return v==IdentificationReason::cold_model?"cold_model":v==IdentificationReason::descriptor_unreadable?"descriptor_unreadable":v==IdentificationReason::model_unconfirmed?"model_unconfirmed":"none";}
bool error(Writer& w,const Optional<ErrorRecord>& v) noexcept
{
    if(!v.present)return w.text("null");
    const char* symbol=dtv::userland::error_symbol(v.value.code);
    if(!w.ch('{')||!key(w,"code")||!w.string(symbol?symbol:"UNKNOWN",symbol?length(symbol):7U))return false;
    if(!symbol && (!w.ch(',')||!key(w,"numeric_code")||!w.number(v.value.code)))return false;
    return w.ch(',')&&key(w,"operation")&&str(w,v.value.operation)&&w.ch(',')&&key(w,"detail")&&str(w,v.value.detail)&&w.ch('}');
}
bool function(Writer& w,const UsbFunction& v) noexcept
{
    const char* m=mode(v.mode); const char* ss=serial_state(v.serial_state); const char* ir=identification_reason(v.identification_reason);
    if(!w.ch('{')||!key(w,"id")||!w.quoted_number(v.id)||!w.ch(',')||!key(w,"vid"))return false;
    static constexpr char h[]="0123456789abcdef";
    if(!w.ch('"')||!w.ch(h[(v.vid>>12U)&15U])||!w.ch(h[(v.vid>>8U)&15U])||!w.ch(h[(v.vid>>4U)&15U])||!w.ch(h[v.vid&15U])||!w.ch('"')||!w.ch(',')||!key(w,"pid"))return false;
    if(!w.ch('"')||!w.ch(h[(v.pid>>12U)&15U])||!w.ch(h[(v.pid>>8U)&15U])||!w.ch(h[(v.pid>>4U)&15U])||!w.ch(h[v.pid&15U])||!w.ch('"')||!w.ch(',')||!key(w,"mode")||!w.string(m,length(m))||!w.ch(',')||!key(w,"location")||!w.ch('{')||!key(w,"usb_path")||(v.location.usb_path.present?!str(w,v.location.usb_path.value):!w.text("null"))||!w.ch(',')||!key(w,"bus")||(v.location.bus.present?!w.number(v.location.bus.value):!w.text("null"))||!w.ch(',')||!key(w,"address")||(v.location.address.present?!w.number(v.location.address.value):!w.text("null"))||!w.ch('}')||!w.ch(',')||!key(w,"serial")||!w.ch('{')||!key(w,"state")||!w.string(ss,length(ss))||!w.ch(',')||!key(w,"value")||(v.serial.present?!str(w,v.serial.value):!w.text("null"))||!w.ch('}')||!w.ch(',')||!key(w,"identification")||!w.ch('{')||!key(w,"model")||!model(w,v.model)||!w.ch(',')||!key(w,"reason")||(v.identification_reason==IdentificationReason::none?!w.text("null"):!w.string(ir,length(ir)))||!w.ch('}')||!w.ch(',')||!key(w,"present")||!boolean(w,v.present)||!w.ch(',')||!key(w,"last_error")||!error(w,v.last_error)||!w.ch('}'))return false;
    return true;
}
bool binding(Writer& w,const Optional<Binding>& v) noexcept
{return !v.present?w.text("null"):(w.ch('{')&&key(w,"function_id")&&w.quoted_number(v.value.function_id)&&w.ch(',')&&key(w,"local_id")&&w.quoted_number(v.value.local_id)&&w.ch('}'));}
bool capabilities(Writer& w,const Capabilities& v) noexcept
{
    if(!w.ch('{')||!key(w,"ts")||!w.ch('{')||!key(w,"scope")||!w.text(v.ts_scope==TsScope::pid_subset?"\"pid_subset\"":"\"multiplex\"")||!w.ch(',')||!key(w,"pid_filter"))return false;
    if(v.max_pids.present){if(!w.ch('{')||!key(w,"max_pids")||!w.number(v.max_pids.value)||!w.ch('}'))return false;}else if(!w.text("null"))return false;
    if(!w.ch('}')||!w.ch(',')||!key(w,"controls")||!w.ch('{'))return false;
    bool comma=false;
    if((v.controls&kControlBitLna)!=0U){if(!key(w,"lna")||!w.ch('{')||!key(w,"values")||!w.text("[false,true]")||!w.ch(',')||!key(w,"default_by_system")||!w.ch('{'))return false;for(std::size_t i=0;i<v.lna.entries.count;++i){const auto& e=v.lna.entries.items[i];const char* name=e.system==System::isdb_s?"isdb-s":"isdb-t";if(i&& !w.ch(','))return false;if(!key(w,name)||(e.default_value.present?!boolean(w,e.default_value.value):!w.text("null")))return false;}if(!w.ch('}')||!w.ch('}'))return false;comma=true;}
    if((v.controls&kControlBitLnb)!=0U){if(comma&&!w.ch(','))return false;if(!key(w,"lnb_voltage")||!w.ch('{')||!key(w,"values")||!w.ch('['))return false;for(std::size_t i=0;i<v.lnb.values.count;++i){if(i&&!w.ch(','))return false;if(!w.number(v.lnb.values.items[i]))return false;}if(!w.text("]}") )return false;}
    if(!w.ch('}')||!w.ch(',')||!key(w,"measurements")||!w.ch('{'))return false;
    bool first=true;const char* names[]={"lock","cnr_mdb","signal_mdbm"};const char* units[]={"boolean","milli_dB","milli_dBm"};
    for(std::size_t i=0;i<3U;++i)if((v.measurement_mask&(1U<<i))!=0U){if(!first&&!w.ch(','))return false;first=false;if(!key(w,names[i])||!w.ch('{')||!key(w,"unit")||!w.string(units[i],length(units[i]))||!w.ch('}'))return false;}
    return w.ch('}')&&w.ch('}');
}
bool receiver_descriptor(Writer& w,const ReceiverDescriptor& v) noexcept
{
    if(!w.ch('{')||!key(w,"id")||!w.quoted_number(v.id)||!w.ch(',')||!key(w,"systems")||!w.ch('['))return false;
    bool comma=false;if((v.systems.mask&1U)!=0U){if(!w.text("\"isdb-t\""))return false;comma=true;}if((v.systems.mask&2U)!=0U){if(comma&&!w.ch(','))return false;if(!w.text("\"isdb-s\""))return false;}
    return w.ch(']')&&w.ch(',')&&key(w,"binding")&&binding(w,v.binding)&&w.ch(',')&&key(w,"capabilities")&&capabilities(w,v.capabilities)&&w.ch('}');
}
bool reader_descriptor(Writer& w,const ReaderDescriptor& v) noexcept
{return w.ch('{')&&key(w,"id")&&w.quoted_number(v.id)&&w.ch(',')&&key(w,"binding")&&binding(w,v.binding)&&w.ch('}');}
bool metric(Writer& w,const MetricEntry& v) noexcept
{
    const bool lock=v.id==kMetricIdLock;const auto state=lock?v.boolean.state:v.integer.state;
    if(state==MeasureState::unsupported)return w.text("null");
    if(!w.ch('{')||!key(w,"value"))return false;
    if(state==MeasureState::unknown){if(!w.text("null")||!w.ch(',')||!key(w,"sampled_at_ns")||!w.text("null")||!w.ch(',')||!key(w,"reason"))return false;const auto reason=lock?v.boolean.reason:v.integer.reason;const char* s=reason==UnknownReason::stale?"stale":reason==UnknownReason::tune_changed?"tune_changed":reason==UnknownReason::device_lost?"device_lost":reason==UnknownReason::read_failed?"read_failed":"not_sampled";return w.string(s,length(s))&&w.ch('}');}
    if(!(lock?boolean(w,v.boolean.value):w.signed_number(v.integer.value))||!w.ch(',')||!key(w,"sampled_at_ns")||!w.quoted_number(lock?v.boolean.sampled_at_ns:v.integer.sampled_at_ns)||!w.ch(',')||!key(w,"reason")||!w.text("null"))return false;
    return w.ch('}');
}
bool measurements(Writer& w,const Measurements& v) noexcept
{
    if(!w.ch('{'))return false;
    bool comma=false;const char* names[]={"lock","cnr_mdb","signal_mdbm"};
    for(std::size_t i=0;i<v.entries.count;++i){const auto& e=v.entries.items[i];if(e.id==0U||e.id>3U)continue;if(comma&&!w.ch(','))return false;comma=true;if(!key(w,names[e.id-1U])||!metric(w,e))return false;}return w.ch('}');
}
const char* counter_name(std::uint8_t id) noexcept {switch(id){case 1:return "captured_packets";case 2:return "usb_errors";case 3:return "framing_errors";case 4:return "upstream_lost_packets";case 5:return "transmitted_packets";default:return "queue_dropped_packets";}}
bool counter(Writer& w,const Counter& v) noexcept
{
    const auto& m=v.measurement;if(!w.ch('{')||!key(w,"value")||!nullable_u64(w,Optional<std::uint64_t>{m.state==MeasureState::valid,m.value})||!w.ch(',')||!key(w,"sampled_at_ns")||!(m.state==MeasureState::valid?w.quoted_number(m.sampled_at_ns):w.text("null"))||!w.ch(',')||!key(w,"reason"))return false;
    if(m.state==MeasureState::valid){if(!w.text("null"))return false;}else{const char* s=m.reason==UnknownReason::stale?"stale":m.reason==UnknownReason::tune_changed?"tune_changed":m.reason==UnknownReason::device_lost?"device_lost":m.reason==UnknownReason::read_failed?"read_failed":"not_sampled";if(!w.string(s,length(s)))return false;}
    return w.ch(',')&&key(w,"overflow")&&(m.state==MeasureState::valid?boolean(w,v.overflow):w.text("null"))&&w.ch('}');
}
template <typename A> bool counters(Writer& w,const A& v) noexcept
{if(!w.ch('{'))return false;for(std::size_t i=0;i<v.entries.count;++i){if(i&&!w.ch(','))return false;if(!key(w,counter_name(v.entries.items[i].id))||!counter(w,v.entries.items[i].counter))return false;}return w.ch('}');}
bool selection(Writer& w,const Tune& v) noexcept
{if(!w.ch('{')||!key(w,"kind"))return false;const char* k=v.selection==Selection::tsid?"tsid":v.selection==Selection::slot?"slot":"auto";if(!w.string(k,length(k)))return false;if(v.selection!=Selection::automatic)return w.ch(',')&&key(w,"value")&&w.number(v.selection_value)&&w.ch('}');return w.ch('}');}
bool tune(Writer& w,const Optional<Tune>& v) noexcept
{if(!v.present)return w.text("null");const auto& t=v.value;const char* sys=t.system==System::isdb_s?"isdb-s":"isdb-t";const char* ver=t.verification==Verification::lock?"lock":t.verification==Verification::ts_sync?"ts_sync":"commands";return w.ch('{')&&key(w,"revision")&&w.quoted_number(t.revision)&&w.ch(',')&&key(w,"system")&&w.string(sys,length(sys))&&w.ch(',')&&key(w,"frequency_hz")&&w.number(t.frequency_hz)&&w.ch(',')&&key(w,"programmed_hz")&&(t.programmed_hz.present?w.number(t.programmed_hz.value):w.text("null"))&&w.ch(',')&&key(w,"selection")&&selection(w,t)&&w.ch(',')&&key(w,"verification")&&w.string(ver,length(ver))&&w.ch('}');}
bool stream(Writer& w,const Optional<Stream>& v) noexcept
{if(!v.present)return w.text("null");const char* p=v.value.phase==StreamPhase::attaching?"attaching":v.value.phase==StreamPhase::active?"active":v.value.phase==StreamPhase::closing?"closing":"armed";return w.ch('{')&&key(w,"id")&&w.quoted_number(v.value.id)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(v.value.tune_revision)&&w.ch(',')&&key(w,"phase")&&w.string(p,length(p))&&w.ch('}');}
const char* end_reason(StreamEndReason v) noexcept {switch(v){case StreamEndReason::data_closed:return "data_closed";case StreamEndReason::owner_closed:return "owner_closed";case StreamEndReason::attach_expired:return "attach_expired";case StreamEndReason::slow_consumer:return "slow_consumer";case StreamEndReason::device_lost:return "device_lost";case StreamEndReason::backend_error:return "backend_error";case StreamEndReason::shutdown:return "shutdown";default:return "stopped";}}
bool final_stream(Writer& w,const Optional<StreamFinal>& v) noexcept
{if(!v.present)return w.text("null");const auto& f=v.value;return w.ch('{')&&key(w,"id")&&w.quoted_number(f.id)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(f.tune_revision)&&w.ch(',')&&key(w,"reason")&&w.string(end_reason(f.reason),length(end_reason(f.reason)))&&w.ch(',')&&key(w,"error")&&error(w,f.error)&&w.ch(',')&&key(w,"statistics")&&counters(w,f.statistics)&&w.ch('}');}
bool receiver_status(Writer& w,const ReceiverStatus& v) noexcept
{
    static const char* states[]={"free","leased","tuned","streaming","error","absent"};static const char* ops[]={"idle","tuning","stopping","releasing"};
    return w.ch('{')&&key(w,"descriptor")&&receiver_descriptor(w,v.descriptor)&&w.ch(',')&&key(w,"present")&&boolean(w,v.present)&&w.ch(',')&&key(w,"state")&&w.string(states[static_cast<unsigned>(v.state)],length(states[static_cast<unsigned>(v.state)]))&&w.ch(',')&&key(w,"operation")&&w.string(ops[static_cast<unsigned>(v.operation)],length(ops[static_cast<unsigned>(v.operation)]))&&w.ch(',')&&key(w,"leased")&&boolean(w,v.leased)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(v.tune_revision)&&w.ch(',')&&key(w,"current_tune")&&tune(w,v.current_tune)&&w.ch(',')&&key(w,"last_successful_tune")&&tune(w,v.last_successful_tune)&&w.ch(',')&&key(w,"stream")&&stream(w,v.stream)&&w.ch(',')&&key(w,"last_stream")&&final_stream(w,v.last_stream)&&w.ch(',')&&key(w,"measurements")&&measurements(w,v.measurements)&&w.ch(',')&&key(w,"statistics")&&w.ch('{')&&key(w,"receiver")&&counters(w,v.receiver_statistics)&&w.ch(',')&&key(w,"stream")&&(v.stream_statistics.present?counters(w,v.stream_statistics.value):w.text("null"))&&w.ch('}')&&w.ch(',')&&key(w,"last_error")&&error(w,v.last_error)&&w.ch('}');
}
const char* card_state(CardState v) noexcept{return v==CardState::absent?"absent":v==CardState::present?"present":v==CardState::unavailable?"unavailable":"unknown";}
const char* card_reason(std::uint8_t v) noexcept{return v==2U?"stale":v==4U?"device_lost":v==5U?"read_failed":"not_sampled";}
bool reader_status(Writer& w,const ReaderStatus& v) noexcept
{static const char* ops[]={"idle","initializing","transmitting","resetting","stopping"};const char* cs=card_state(v.card.state);const bool sampled=v.card.sampled_at_ns.present;return w.ch('{')&&key(w,"descriptor")&&reader_descriptor(w,v.descriptor)&&w.ch(',')&&key(w,"present")&&boolean(w,v.present)&&w.ch(',')&&key(w,"card")&&w.ch('{')&&key(w,"state")&&w.string(cs,length(cs))&&w.ch(',')&&key(w,"sampled_at_ns")&&nullable_u64(w,v.card.sampled_at_ns)&&w.ch(',')&&key(w,"reason")&&(sampled?w.text("null"):w.string(card_reason(v.card.reason),length(card_reason(v.card.reason))))&&w.ch('}')&&w.ch(',')&&key(w,"operation")&&w.string(ops[static_cast<unsigned>(v.operation)],length(ops[static_cast<unsigned>(v.operation)]))&&w.ch(',')&&key(w,"handles_open")&&w.number(v.handles_open)&&w.ch(',')&&key(w,"transaction_active")&&boolean(w,v.transaction_active)&&w.ch(',')&&key(w,"last_error")&&error(w,v.last_error)&&w.ch('}');}

bool emit(const StatusResponse& v,Writer& w) noexcept
{
    static constexpr char hex[]="0123456789abcdef";const char* states[]={"starting","ready","stopping","error","disconnected"};
    if(!w.ch('{')||!key(w,"schema")||!w.text("{\"name\":\"userland.status\",\"major\":1,\"minor\":0}")||!w.ch(',')||!key(w,"producer")||!producer(w,v.producer)||!w.ch(',')||!key(w,"context")||!w.ch('{')||!key(w,"epoch")||!w.ch('"'))return false;
    for(auto b:v.context.epoch){if(!w.ch(hex[(b>>4U)&15U])||!w.ch(hex[b&15U]))return false;}
    if(!w.ch('"')||!w.ch(',')||!key(w,"instance")||(v.context.instance.present?!str(w,v.context.instance.value):!w.text("null"))||!w.ch(',')||!key(w,"state")||!w.string(states[static_cast<unsigned>(v.context.state)],length(states[static_cast<unsigned>(v.context.state)]))||!w.ch(',')||!key(w,"last_error")||!error(w,v.context.last_error)||!w.ch('}')||!w.ch(',')||!key(w,"snapshot")||!w.ch('{')||!key(w,"sequence")||!w.quoted_number(v.snapshot.sequence)||!w.ch(',')||!key(w,"observed_at_ns")||!w.quoted_number(v.snapshot.observed_at_ns)||!w.ch('}')||!w.ch(',')||!key(w,"scope")||!w.ch('{')||!key(w,"kind"))return false;
    const char* scope=v.scope.kind==ScopeKind::receiver?"receiver":v.scope.kind==ScopeKind::reader?"reader":v.scope.kind==ScopeKind::stream?"stream":"enclosure";
    if(!w.string(scope,length(scope))||!w.ch(',')||!key(w,"id")||(v.scope.kind==ScopeKind::enclosure?!w.text("null"):!w.quoted_number(v.scope.id))||!w.ch('}')||!w.ch(',')||!key(w,"enclosure")||!enclosure(w,v.enclosure)||!w.ch(',')||!key(w,"topology")||!topology(w,v.topology))return false;
    if(v.functions.present){if(!w.ch(',')||!key(w,"usb_functions")||!w.ch('['))return false;std::array<std::uint8_t,kMaxFunctions> order{};for(std::size_t i=0;i<v.functions.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.functions.value.count;++i)for(std::size_t j=i+1;j<v.functions.value.count;++j)if(v.functions.value.items[order[j]].id<v.functions.value.items[order[i]].id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.functions.value.count;++i){if(i&&!w.ch(','))return false;if(!function(w,v.functions.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    if(v.receivers.present){if(!w.ch(',')||!key(w,"receivers")||!w.ch('['))return false;std::array<std::uint8_t,kMaxPublicReceivers> order{};for(std::size_t i=0;i<v.receivers.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.receivers.value.count;++i)for(std::size_t j=i+1;j<v.receivers.value.count;++j)if(v.receivers.value.items[order[j]].descriptor.id<v.receivers.value.items[order[i]].descriptor.id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.receivers.value.count;++i){if(i&&!w.ch(','))return false;if(!receiver_status(w,v.receivers.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    if(v.readers.present){if(!w.ch(',')||!key(w,"readers")||!w.ch('['))return false;std::array<std::uint8_t,kMaxReaders> order{};for(std::size_t i=0;i<v.readers.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.readers.value.count;++i)for(std::size_t j=i+1;j<v.readers.value.count;++j)if(v.readers.value.items[order[j]].descriptor.id<v.readers.value.items[order[i]].descriptor.id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.readers.value.count;++i){if(i&&!w.ch(','))return false;if(!reader_status(w,v.readers.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    return w.text("}\n");
}

}  // namespace

Result<std::size_t> status_json_size(const StatusResponse& value,const StatusContext& context) noexcept
{
    const Error valid=validate_status_response(value,context);if(valid!=Error::OK)return Result<std::size_t>::failure(valid);
    Writer count({nullptr,0U});if(!emit(value,count))return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_status_json(const StatusResponse& value,const StatusContext& context,MutableByteView output) noexcept
{
    const Error valid=validate_status_response(value,context);if(valid!=Error::OK)return Result<std::size_t>::failure(valid);
    if(output.data==nullptr&&output.size!=0U)return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr,0U});if(!emit(value,count))return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if(output.size<count.size())return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer write(output);if(!emit(value,write)||write.size()!=count.size())return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(write.size());
}

}  // namespace dtv::userland::wire
