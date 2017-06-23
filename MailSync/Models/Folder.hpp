//
//  Folder.hpp
//  MailSync
//
//  Created by Ben Gotow on 6/17/17.
//  Copyright © 2017 Foundry 376. All rights reserved.
//

#ifndef Folder_hpp
#define Folder_hpp

#include <stdio.h>
#include <string>
#include "json.hpp"

#include "MailModel.hpp"

using json = nlohmann::json;
using namespace std;

class Folder : public MailModel {
    
public:
    static string TABLE_NAME;

    Folder(json & json);
    Folder(string id, string accountId, int version);
    Folder(SQLite::Statement & query);

    json & localStatus();
    
    string path();
    void setPath(string path);
    
    string role() const;
    void setRole(string role);
  
    string tableName();
    vector<string> columnsForQuery();
    void bindToQuery(SQLite::Statement & query);
};

#endif /* Folder_hpp */
