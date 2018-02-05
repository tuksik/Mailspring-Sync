//
//  MailProcessor.cpp
//  MailSync
//
//  Created by Ben Gotow on 6/20/17.
//  Copyright © 2017 Foundry 376. All rights reserved.
//
//  Use of this file is subject to the terms and conditions defined
//  in 'LICENSE.md', which is part of the Mailspring-Sync package.
//

#include "MailProcessor.hpp"
#include "MailStoreTransaction.hpp"
#include "MailUtils.hpp"
#include "File.hpp"
#include "constants.h"

using namespace std;
using nlohmann::json;

class CleanHTMLBodyRendererTemplateCallback : public Object, public HTMLRendererTemplateCallback {
    mailcore::String * templateForMainHeader(MessageHeader * header) {
        return MCSTR("");
    }

    mailcore::String * templateForAttachment(AbstractPart * part) {
        return MCSTR("");
    }

    mailcore::String * templateForAttachmentSeparator() {
        return MCSTR("");
    }

    // Normally this calls through to XMLTidy but we have our own sanitizer at the Javascript
    // level and Tidy has led to some bugs due to its very strict parsing:
    // https://github.com/Foundry376/Mailspring/issues/301#issuecomment-342265351
    mailcore::String * cleanHTMLForPart(mailcore::String * html) {
        return html;
    }

    // TODO: Image attachments can be added to the middle of messages
    // by putting them between two HTML parts and we can render them
    // within the body this way. However the attachments don't have cid's,
    // and the client expects to filter attachments based on whether they
    // have cids.
    
//    mailcore::String * templateForImage(AbstractPart * part) {
//        MailUtils::idForFilePart(part)
//        return MCSTR("<img src=\"{{CONTENTID}}\" data-size=\"{{SIZE}}\" data-filename=\"{{FILENAME}}\" />");
//    }
//
//    bool canPreviewPart(AbstractPart * part) {
//        string t = part->mimeType()->UTF8Characters();
//        
//        if ((t == "image/png") || (t == "image/jpeg") || (t == "image/jpg") || (t == "image/gif")) {
//            return true;
//        }
//        return false;
//    }
};


MailProcessor::MailProcessor(shared_ptr<Account> account, MailStore * store) :
    store(store),
    account(account),
    logger(spdlog::get("logger"))
{

}

shared_ptr<Message> MailProcessor::insertFallbackToUpdateMessage(IMAPMessage * mMsg, Folder & folder, time_t syncDataTimestamp) {
    try {
        return insertMessage(mMsg, folder, syncDataTimestamp);
    } catch (const SQLite::Exception & ex) {
        if (ex.getErrorCode() != 19) { // constraint failed
            throw;
        }
        Query q = Query().equal("id", MailUtils::idForMessage(folder.accountId(), folder.path(), mMsg));
        auto localMessage = store->find<Message>(q);
        if (localMessage.get() == nullptr) {
            throw;
        }
        updateMessage(localMessage.get(), mMsg, folder, syncDataTimestamp);
        return localMessage;
    }
}

shared_ptr<Message> MailProcessor::insertMessage(IMAPMessage * mMsg, Folder & folder, time_t syncDataTimestamp) {
    shared_ptr<Message> msg = make_shared<Message>(mMsg, folder, syncDataTimestamp);
    shared_ptr<Thread> thread = nullptr;

    Array * references = mMsg->header()->references();
    if (references == nullptr) {
        references = new Array();
        references->autorelease();
    }

    {
        MailStoreTransaction transaction{store};

        // Find the correct thread

        if (mMsg->gmailThreadID()) {
            Query query = Query().equal("gThrId", to_string(mMsg->gmailThreadID()));
            thread = store->find<Thread>(query);
            
        } else if (!mMsg->header()->isMessageIDAutoGenerated()) {
            // find an existing thread using the references. Note - a rouge client could
            // throw a lot of shit in here, limit the number of refs we look at to 50.
            // TODO: It appears we should technically use the first 1 and then last 49.
            int refcount = min(50, (int)references->count());
            SQLite::Statement tQuery(store->db(), "SELECT Thread.* FROM Thread INNER JOIN ThreadReference ON ThreadReference.threadId = Thread.id WHERE ThreadReference.accountId = ? AND ThreadReference.headerMessageId IN (" + MailUtils::qmarks(1 + refcount) + ") LIMIT 1");
            tQuery.bind(1, msg->accountId());
            tQuery.bind(2, msg->headerMessageId());
            for (int i = 0; i < refcount; i ++) {
                String * ref = (String *)references->objectAtIndex(i);
                tQuery.bind(3 + i, ref->UTF8Characters());
            }
            if (tQuery.executeStep()) {
                thread = make_shared<Thread>(tQuery);
            }
        }
        
        if (thread == nullptr) {
            // TODO: could move to message save hooks
            thread = make_shared<Thread>(msg->id(), account->id(), msg->subject(), mMsg->gmailThreadID());
        }
        
        msg->setThreadId(thread->id());

        // Index the thread metadata for search. We only do this once and it'd
        // be costly to make it part of the save hooks.
        appendToThreadSearchContent(thread.get(), msg.get(), nullptr);
        store->save(thread.get());

        // Save the message - this will automatically find and update the counters
        // on the thread we just created. Kind of a shame to find it twice but oh well.
        store->save(msg.get());
        
        // Make the thread accessible by all of the message references
        upsertThreadReferences(thread->id(), thread->accountId(), msg->headerMessageId(), references);

        // Index contacts for autocomplete
        upsertContacts(msg.get());

        transaction.commit();
    }
    
    return msg;
}

void MailProcessor::updateMessage(Message * local, IMAPMessage * remote, Folder & folder, time_t syncDataTimestamp)
{
    if (local->syncedAt() > syncDataTimestamp) {
        logger->warn("Ignoring changes to {}, local data is newer {} < {}", local->subject(), syncDataTimestamp, local->syncedAt());
        return;
    }
    
    auto updated = MessageAttributesForMessage(remote);
    auto jlabels = json(updated.labels);

    logger->info("- Updating message {}", local->id());
    
    bool noChanges = true;
    if (updated.unread != local->isUnread()) {
        logger->info("-- Unread ({} to {})", local->isUnread(), updated.unread);
        noChanges = false;
    }
    if (updated.starred != local->isStarred()) {
        logger->info("-- Starred ({} to {})", local->isStarred(), updated.starred);
        noChanges = false;
    }
    if (updated.draft != local->isDraft()) {
        logger->info("-- Starred ({} to {})", local->isDraft(), updated.draft);
        noChanges = false;
    }
    if (updated.uid != local->remoteUID()) {
        logger->info("-- UID ({} to {})", local->remoteUID(), updated.uid);
        noChanges = false;
    }
    if (folder.id() != local->remoteFolderId()) {
        logger->info("-- FolderID ({} to {})", local->remoteFolderId(), folder.id());
        noChanges = false;
    }
    if (jlabels != local->remoteXGMLabels()) {
        logger->info("-- XGMLabels ({} to {})", local->remoteXGMLabels().dump(), jlabels.dump());
        noChanges = false;
    }

    if (noChanges) {
        return;
    }

    {
        MailStoreTransaction transaction{store};
    
        local->setUnread(updated.unread);
        local->setStarred(updated.starred);
        local->setDraft(updated.draft);
        local->setRemoteUID(updated.uid);
        local->setRemoteFolder(&folder);
        local->setSyncedAt(syncDataTimestamp);
        local->setClientFolder(&folder);
        local->setRemoteXGMLabels(jlabels);
        
        // Save the message - this will automatically find and update the counters
        // on the thread we just created. Kind of a shame to find it twice but oh well.
        store->save(local);

        transaction.commit();
    }
}

void MailProcessor::retrievedMessageBody(Message * message, MessageParser * parser) {
    CleanHTMLBodyRendererTemplateCallback * callback = new CleanHTMLBodyRendererTemplateCallback();
    String * html = parser->htmlRendering(callback);
    String * text = html->flattenHTML()->stripWhitespace();
    MC_SAFE_RELEASE(callback);

    const char * chars = html->UTF8Characters();

    // build file containers for the attachments and write them to disk
    Array attachments = Array();
    attachments.addObjectsFromArray(parser->attachments());
    attachments.addObjectsFromArray(parser->htmlInlineAttachments());
    
    vector<File> files;
    for (int ii = 0; ii < attachments.count(); ii ++) {
        Attachment * a = (Attachment *)attachments.objectAtIndex(ii);
        File f = File(message, a);
        
        bool duplicate = false;
        for (auto & other : files) {
            if (other.partId() == string(a->partID()->UTF8Characters())) {
                duplicate = true;
                logger->info("Attachment is duplicate: {}", f.toJSON().dump());
                break;
            }
        }
        
        // Sometimes the HTML will reference "cid:filename.png@123123garbage" and the file will
        // not have a contentId. The client does not support this, so if cid:filename.png appears
        // in the body we manually make it the contentId
        if (f.contentId().is_null() && strstr(chars, ("cid:" + f.filename()).c_str()) != nullptr) {
            f.setContentId(f.filename());
        }

        if (!duplicate) {
            if (!retrievedFileData(&f, a->data())) {
                logger->info("Could not save file data!");
            }
            files.push_back(f);
        }
    }
    
    // enter transaction
    {
        MailStoreTransaction transaction{store};
        
        // write body to the MessageBodies table
        SQLite::Statement insert(store->db(), "REPLACE INTO MessageBody (id, value, fetchedAt) VALUES (?, ?, datetime('now'))");
        insert.bind(1, message->id());
        insert.bind(2, chars);
        insert.exec();
        
        // write files to the files table
        
        // try to save the files to the database. We don't care about failures here -
        // it's possible the files are already there if we're re-fetching this message
        // for some reason and we haven't loaded the existing ones.
        for (auto & file : files) {
            try {
                store->save(&file);
            } catch (SQLite::Exception & ex) {
                logger->warn("Unable to insert file ID {} - it must already exist.", file.id());
            }
        }
        
        // append the body text to the thread's FTS5 search index
        auto thread = store->find<Thread>(Query().equal("id", message->threadId()));
        if (thread.get() != nullptr) {
            appendToThreadSearchContent(thread.get(), nullptr, text);
        }

        // write the message snippet. This also gives us the database trigger!
        message->setSnippet(text->substringToIndex(400)->UTF8Characters());
        message->setBodyForDispatch(chars);
        message->setFiles(files);
        
        store->save(message);
        
        transaction.commit();
    }
}


bool MailProcessor::retrievedFileData(File * file, Data * data) {
    string root = MailUtils::getEnvUTF8("CONFIG_DIR_PATH") + FS_PATH_SEP + "files";
    string path = MailUtils::pathForFile(root, file, true);
    return (data->writeToFile(AS_MCSTR(path)) == ErrorNone);
}

void MailProcessor::unlinkMessagesMatchingQuery(Query & query, int phase)
{
    // Note: This method may be called with a Query() returning the entire folder
    // in case of UIDInvalidity. Loading + saving is super inefficient in this rare case,
    // but the field is currently both in the JSON and in a separate column. In the future
    // we may want to make the column the sole source of truth, but it looks like a
    // complicated change because _data is used for cloning models, etc and inflation
    // is very abstracted.
    
    logger->info("Unlinking messages {} no longer present in remote range.", query.getSQL());
    
    {
        MailStoreTransaction transaction{store};

        auto deletedMsgs = store->findAll<Message>(query);
        bool logSubjects = deletedMsgs.size() < 40;

        logger->info("-- {} matches.", deletedMsgs.size());

        for (const auto msg : deletedMsgs) {
            if (msg->remoteUID() > UINT32_MAX - 5) {
                // we unlinked this message in a previous cycle and it will be deleted momentarily.
                continue;
            }
            
            // don't spam the logs when a zillion messages are being deleted
            if (logSubjects) {
                logger->info("-- Unlinking \"{}\" ({})", msg->subject(), msg->id());
            }
            msg->setRemoteUID(UINT32_MAX - phase);
            
            // we know we don't need to emit this change because the client can't see the remoteUID
            store->save(msg.get(), false);
        }

        transaction.commit();
    }
}

void MailProcessor::deleteMessagesStillUnlinkedFromPhase(int phase)
{
    bool more = true;
    int chunkSize = 100;
    
    // If the user deletes a zillion messages, this function can take a long
    // time and block the database. Break it up a bit!

    while (more) {
        MailStoreTransaction transaction{store};
        
        auto q = Query().equal("accountId", account->id()).equal("remoteUID", UINT32_MAX - phase).limit(chunkSize);
        auto messages = store->findAll<Message>(q);
        if (messages.size() < chunkSize){
            more = false;
        }
        
        for (auto const & msg : messages) {
            logger->info("-- Removing \"{}\" ({})", msg->subject(), msg->id());
            store->remove(msg.get());
        }
        
        // send the deltas
        transaction.commit();
    }
}

void MailProcessor::appendToThreadSearchContent(Thread * thread, Message * messageToAppendOrNull, String * bodyToAppendOrNull) {
    // retrieve the current index if there is one
    
    string to;
    string from;
    string categories = thread->categoriesSearchString();
    string body = thread->subject();
    
    if (thread->searchRowId()) {
        SQLite::Statement existing(store->db(), "SELECT to_, from_, body FROM ThreadSearch WHERE rowid = ?");
        existing.bind(1, (double)thread->searchRowId());
        if (existing.executeStep()) {
            to = existing.getColumn("to_").getString();
            from = existing.getColumn("from_").getString();
            body = existing.getColumn("body").getString();
        }
    }
    
    if (messageToAppendOrNull != nullptr) {
        for (auto c : messageToAppendOrNull->to()) {
            if (c.count("email")) { to = to + " " + c["email"].get<string>(); }
            if (c.count("name")) { to = to + " " + c["name"].get<string>(); }
        }
        for (auto c : messageToAppendOrNull->cc()) {
            if (c.count("email")) { to = to + " " + c["email"].get<string>(); }
            if (c.count("name")) { to = to + " " + c["name"].get<string>(); }
        }
        for (auto c : messageToAppendOrNull->bcc()) {
            if (c.count("email")) { to = to + " " + c["email"].get<string>(); }
            if (c.count("name")) { to = to + " " + c["name"].get<string>(); }
        }
        for (auto c : messageToAppendOrNull->from()) {
            if (c.count("email")) { from = from + " " + c["email"].get<string>(); }
            if (c.count("name")) { from = from + " " + c["name"].get<string>(); }
        }
    }
    
    if (bodyToAppendOrNull != nullptr) {
        body = body + " " + bodyToAppendOrNull->substringToIndex(5000)->UTF8Characters();
    }
    
    if (thread->searchRowId()) {
        SQLite::Statement update(store->db(), "UPDATE ThreadSearch SET to_ = ?, from_ = ?, body = ?, categories = ? WHERE rowid = ?");
        update.bind(1, to);
        update.bind(2, from);
        update.bind(3, body);
        update.bind(4, categories);
        update.bind(5, (double)thread->searchRowId());
        update.exec();
    } else {
        SQLite::Statement insert(store->db(), "INSERT INTO ThreadSearch (to_, from_, body, categories, content_id) VALUES (?, ?, ?, ?, ?)");
        insert.bind(1, to);
        insert.bind(2, from);
        insert.bind(3, body);
        insert.bind(4, categories);
        insert.bind(5, thread->id());
        insert.exec();
        thread->setSearchRowId(store->db().getLastInsertRowid());
    }
}

void MailProcessor::upsertThreadReferences(string threadId, string accountId, string headerMessageId, Array * references) {
    SQLite::Statement query(store->db(), "INSERT OR IGNORE INTO ThreadReference (threadId, accountId, headerMessageId) VALUES (?,?,?)");
    query.bind(1, threadId);
    query.bind(2, accountId);
    query.bind(3, headerMessageId);
    query.exec();
    query.reset();

    // todo: technically, we should look at the first reference (Start of thread)
    // and then the last N, where N is some number we give a shit about, but we've
    // rarely seen more than 100 items.
    for (int i = 0; i < min(100, (int)references->count()); i ++) {
        String * address = (String*)references->objectAtIndex(i);
        query.bind(3, address->UTF8Characters());
        query.exec();
        query.reset(); // does not clear bindings 1 and 2! https://sqlite.org/c3ref/reset.html
    }
}

void MailProcessor::upsertContacts(Message * message) {
    map<string, json> byEmail{};
    for (auto & c : message->to()) {
        if (c.count("email")) {
            byEmail[MailUtils::contactKeyForEmail(c["email"].get<string>())] = c;
        }
    }
    for (auto & c : message->cc()) {
        if (c.count("email")) {
            byEmail[MailUtils::contactKeyForEmail(c["email"].get<string>())] = c;
        }
    }
    for (auto & c : message->from()) {
        if (c.count("email")) {
            byEmail[MailUtils::contactKeyForEmail(c["email"].get<string>())] = c;
        }
    }
    
    // contactKeyForEmail returns "" for some emails. Toss out that item
    if (byEmail.count("")) {
        byEmail.erase("");
    }
    
    vector<string> emails{};
    for (auto const& imap: byEmail) {
        emails.push_back(imap.first);
    }

    if (emails.size() > 25) {
        // I think it's safe to say mass emails shouldn't create contacts.
        return;
    }

    Query query = Query().equal("email", emails);
    auto results = store->findAll<Contact>(query);
    bool incrementCounters = message->isSentByUser();
    
    for (auto & result : results) {
        // update refcounts of existing items if this is a sent message
        if (incrementCounters) {
            result->incrementRefs();
            store->save(result.get(), false);
        }
        byEmail.erase(result->email());
    }
    
    if (byEmail.size() == 0) {
        return;
    }

    SQLite::Statement searchInsert(store->db(), "INSERT INTO ContactSearch (content_id, content) VALUES (?, ?)");

    for (auto & result : byEmail) {
        // insert remaining items
        Contact c{message->accountId(), result.first, result.second};
        if (incrementCounters) {
            c.incrementRefs();
        }
        store->save(&c, false);
        
        // also index for search
        searchInsert.bind(1, c.id());
        searchInsert.bind(2, c.searchContent());
        searchInsert.exec();
        searchInsert.reset();
    }
}

