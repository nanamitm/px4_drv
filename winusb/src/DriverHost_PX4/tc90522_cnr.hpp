// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cmath>
#include <cstdint>

namespace px4::tc90522 {

// TC90522系のCNDATから、DriverHostの統計APIで使うmdBを求める。
inline std::int32_t CnrMilliDb(bool satellite, std::uint32_t raw)
{
    double cnr=0;
    if(satellite){
        if(raw<3000)return 0;
        const double p=std::sqrt(static_cast<double>(raw-3000))/64;
        cnr=(-1634.6*p*p*p*p*p)+(14341*p*p*p*p)-(50259*p*p*p)+(88977*p*p)-(89565*p)+58857;
    }else{
        if(!raw)return 0;
        const double p=10*std::log10(5505024/static_cast<double>(raw));
        cnr=(0.024*p*p*p*p)-(1.6*p*p*p)+(39.8*p*p)+(549.1*p)+3096.5;
    }
    return std::isnan(cnr)?0:static_cast<std::int32_t>(cnr);
}
}
