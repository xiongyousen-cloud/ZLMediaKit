/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <iostream>
#include "mk_proxyplayer.h"
#include "Player/PlayerProxy.h"

using namespace std;
using namespace mediakit;

int main() {
    auto ini = mk_ini_create();
    mk_ini_set_option(ini, "in_process_transcode", "1");
    mk_ini_set_option(ini, "transcode.codec", "JPEG");
    mk_ini_set_option(ini, "transcode.width", "32");

    auto proxy = mk_proxy_player_create4("__defaultVhost__", "live", "transcode_test", ini, -1);
    auto &player = *reinterpret_cast<PlayerProxy::Ptr *>(proxy);
    if ((*player)["in_process_transcode"] != "1" || (*player)["transcode.codec"] != "JPEG" || (*player)["transcode.width"] != "32") {
        cerr << "proxy player lost in-process transcode options" << endl;
        return 1;
    }

    mk_proxy_player_release(proxy);
    mk_ini_release(ini);
    cout << "proxy options preserved" << endl;
    return 0;
}
