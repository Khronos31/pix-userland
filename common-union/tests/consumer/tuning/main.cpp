// SPDX-License-Identifier: MIT
#include <dtv/userland/tuning.h>

int main()
{
    dtv::tuning::Request request;
    request.channel = "T13";
    const auto result = dtv::tuning::resolve(request);
    return result && result.value().frequency_hz == 473142857ULL &&
                   result.value().system == dtv::userland::wire::System::isdb_t
               ? 0
               : 1;
}
