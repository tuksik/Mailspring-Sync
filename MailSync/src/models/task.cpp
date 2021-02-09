#include "mailsync/models/task.hpp"
#include "mailsync/mail_utils.hpp"
#include "mailsync/models/thread.hpp"
#include "mailsync/models/message.hpp"

using namespace std;
using namespace mailcore;

string Task::TABLE_NAME = "Task";

Task::Task(string constructorName, string accountId, json taskSpecificData) :
    MailModel(MailUtils::idRandomlyGenerated(), accountId) {
    _data["__cls"] = constructorName;
    _data["status"] = "local";
    for (json::iterator it = taskSpecificData.begin(); it != taskSpecificData.end(); ++it) {
        _data[it.key()] = it.value();
    }
}

Task::Task(json json) : MailModel(json) {

}

Task::Task(SQLite::Statement & query) :
    MailModel(query)
{
}

string Task::status() {
    return _data["status"].get<string>();
}

void Task::setStatus(string s) {
    _data["status"] = s;
}

bool Task::shouldCancel() {
    return _data.count("should_cancel") && _data["should_cancel"].get<bool>();
}

void Task::setShouldCancel() {
    _data["should_cancel"] = true;
}


json Task::error() {
    return _data["error"];
}

void Task::setError(json e) {
    _data["error"] = e;
}

json & Task::data() {
    return _data;
}

string Task::constructorName() {
    return _data["__cls"].get<string>();
}

string Task::tableName() {
    return Task::TABLE_NAME;
}

vector<string> Task::columnsForQuery() {
    return vector<string>{"id", "data", "accountId", "version", "status"};
}

void Task::bindToQuery(SQLite::Statement * query) {
    MailModel::bindToQuery(query);
    query->bind(":status", status());
}