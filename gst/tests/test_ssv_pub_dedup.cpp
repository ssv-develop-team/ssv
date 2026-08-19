#include "gstssvpub.hpp"
#include "ssv_meta.hpp"

#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

SsvTrackedObject
make_object(int track_id, SsvTrackState track_state)
{
    SsvTrackedObject object;
    object.track_id = track_id;
    object.track_state = track_state;
    return object;
}

}  // namespace

int
main()
{
    std::unordered_map<int, std::int64_t> last;

    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 30000, 1000, last));
    assert(last.at(1) == 1000);

    assert(!ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 30000, 2000, last));
    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 30000, 31000, last));

    assert(ssv_pub_should_publish(
        {make_object(2, SSV_TRACK_NEW)}, 30000, 32000, last));
    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED),
         make_object(3, SSV_TRACK_NEW)},
        30000, 33000, last));
    assert(last.at(1) == 33000);
    assert(last.at(3) == 33000);

    assert(ssv_pub_should_publish(
        {make_object(-1, SSV_TRACK_NEW)}, 30000, 34000, last));
    assert(!ssv_pub_should_publish(
        {make_object(-1, SSV_TRACK_NEW)}, 30000, 35000, last));

    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 0, 36000, last));
    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 0, 37000, last));

    std::unordered_map<int, std::int64_t> last_edge;
    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_MATCHED)}, 30000, 1000, last_edge));
    assert(ssv_pub_should_publish(
        {make_object(1, SSV_TRACK_LOST)}, 30000, 2000, last_edge));
    assert(ssv_pub_should_publish(
        {make_object(2, SSV_TRACK_DEAD)}, 30000, 3000, last_edge));
    return 0;
}
