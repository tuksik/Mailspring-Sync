//
//  main.cpp
//  MailSync
//
//  Created by Ben Gotow on 6/15/17.
//  Copyright © 2017 Foundry 376. All rights reserved.
//

#include <iostream>
#include <string>
#include <MailCore/MailCore.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include "json.hpp"
#include "spdlog/spdlog.h"

#include "MailUtils.hpp"
#include "MailStore.hpp"
#include "CommStream.hpp"
#include "SyncWorker.hpp"
#include "Task.hpp"
#include "TaskProcessor.hpp"

using json = nlohmann::json;

CommStream * stream = nullptr;
SyncWorker * bgWorker = nullptr;
SyncWorker * fgWorker = nullptr;


void runForegroundSyncWorker() {
    while(true) {
        // run tasks, sync changes, idle, repeat
        fgWorker->idleCycle();
    }
}

void runBackgroundSyncWorker() {
    bool firstLoop = true;
    std::thread * foreground = nullptr;
    
    while(true) {
        // run in a hard loop until it returns false, indicating continuation
        // is not necessary. Then sync and sleep for a bit. Interval can be long
        // because we're idling in another thread.
        bool moreToSync = true;
        while(moreToSync) {
            moreToSync = bgWorker->syncNow();

            // start the "foreground" idle worker after we've completed a single
            // pass through all the folders. This ensures we have the folder list
            // and the uidnext / highestmodseq etc are populated.
            if (firstLoop) {
                foreground = new std::thread(runForegroundSyncWorker);
                firstLoop = false;
            }
        }
        sleep(120);
    }
}

void runMainThread() {
    auto logger = spdlog::stdout_color_mt("main");

    MailStore store;
    store.addObserver(stream);
    
    TaskProcessor processor{&store, logger, nullptr};
    
    while(true) {
        json packet = stream->waitForJSON();
        
        if (packet.count("type") && packet["type"].get<string>() == "task-queued") {
            packet["task"]["version"] = 0;

            Task task{packet["task"]};
            processor.performLocal(&task);
        }
        
        // interrupt the foreground sync worker to do the remote part of the task
        fgWorker->idleInterrupt();
    }
}

int main(int argc, const char * argv[]) {
    spdlog::set_pattern("%l: [%L] %v");

    stream = new CommStream((char *)"/tmp/cmail.sock");
    bgWorker = new SyncWorker("bg", stream);
    fgWorker = new SyncWorker("fg", stream);

    std::thread t1(runForegroundSyncWorker); // SHOUDL BE BACKGROUND
    runMainThread();
    
    return 0;
}
