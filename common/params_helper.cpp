#include "params_helper.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>

using std::cout;
using std::endl;

namespace params {
  
  static bool is_directory(string path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
      if (st.st_mode & S_IFDIR) {
        return true;
      }
    }
    return false;
  }

  static string current_path() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      return cwd;
    } else {
      return "";
    }
  }

  static bool is_symlink(string path) {
    struct stat st;
    int result;
    result = lstat(path.c_str(), &st);
    if (result == 0) {
      if (S_ISLNK(st.st_mode)) {
        return true;
      } else {
        return false;
      }
    }
    return false;
  }


  static bool exists(string path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
      return true;
    } else {
      return false;
    }
  }

  static void mkdirs_exists_ok(string path) {
    int result = system(("mkdir -p -m 666 " + path).c_str());
    if (!exists(path) || result < 0 ) {
      throw OSError();
    }
  }

  static void remove_all(string path) {
    if (!exists(path)) return;
    struct dirent *de;
    DIR *dr = opendir(path.c_str());
    if (dr == NULL) return;

    while ((de = readdir(dr)) != NULL) {
      string file = string(de->d_name);
      string file_path = path + "/" + file;
      if (file != "." && file != "..") {
        if (is_directory(file_path)) {
          remove_all(file_path);
        } else {  
          remove(file_path.c_str());
        }
      }
    }
    closedir(dr);
    remove(path.c_str());
  }

  static int fsync_dir(const char* path){
    int result = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0){ 
      result = -1; 
      goto cleanup;
    }

    result = fsync(fd);
    if (result < 0) {
      goto cleanup;
    }

    cleanup:
      int result_close = 0;
      if (fd >= 0){ 
        result_close = close(fd);
      }
      if (result_close < 0) {
        return result_close;
      } else {
        return result;
      }
  }

  FileLock::FileLock(string path, bool create) {
    _path = path;
    _create = create;
    _fd = -1;
  }

  void FileLock::acquire() {
    if( _create ) {
      _fd = open(_path.c_str(), O_DIRECTORY|O_WRONLY|O_APPEND|O_CREAT, 0400);
    } else {
      _fd = open(_path.c_str(), O_DIRECTORY|O_WRONLY|O_APPEND, 0400);
    }
    flock(_fd, LOCK_EX); 
  }

  void FileLock::release() {
    if (_fd != -1) {
      close(_fd);
      _fd = -1;
    }
  }

  DBAccessor::DBAccessor(string path) {
    _path = path;
  }

  DBAccessor::~DBAccessor() {}

  vector<string> DBAccessor::keys() {
    _check_entered();
    vector<string> ret_keys;

    map<string,string>::iterator itr;
    for (itr = (*_vals).begin(); itr != (*_vals).end(); itr++) {
      ret_keys.push_back(itr->first);
    }
    return ret_keys; 
  }

  const char* DBAccessor::get(string key) {
    _check_entered();
    const char* ret_str;
 
    if (_vals == NULL) return NULL;

    try {
      ret_str = _vals->at(key).c_str();
      return ret_str;
    } catch (const exception& e) {
      return NULL;
    }
  }

  FileLock* DBAccessor::_get_lock(bool create) {
    string path_arg = _path + "/.lock";
    FileLock* lock = new FileLock(path_arg.c_str(), create);
    lock->acquire();
    return lock;
  }

  map<string, string>* DBAccessor::_read_values_locked() {
    map<string, string>* ret_vals = new map<string,string>;
    std::ifstream reader;
    std::stringstream buffer;

    try {
      string data_path = _data_path();

      struct dirent *de;
      DIR *dr = opendir(data_path.c_str());
      if (dr == NULL) return new map<string,string>;

      while ((de = readdir(dr)) != NULL) {
        string file = string(de->d_name);
        string file_path = data_path + "/" + file;
        if (file != "." && file != "..") {
          reader.open(file_path);
          buffer << reader.rdbuf();
          ret_vals->insert({file, buffer.str()});
          reader.close();
        }
      }
      closedir(dr);

    } catch (const exception& e) {
      return new map<string,string>;
    }
    return ret_vals;
  }

  string DBAccessor::_data_path() {
    string ret_path = _path + "/d";
    return ret_path;
  }

  void DBAccessor::_check_entered() {
    if (_vals == NULL) throw MustEnterDB();
  }

  DBReader::DBReader(string path)
    : DBAccessor(path) {
    _lock = NULL;
    enter();
  }
   
  void DBReader::_delete(string key) {
    return; 
  }

  void DBReader::enter() {
    try {
      _lock = _get_lock(false);
    } catch (const exception& e) {
      _vals = new map<string,string>;
    }

    try {
      _vals = _read_values_locked();
    } catch(const exception& e) {goto finally;}
    goto finally;
    finally:
      _lock->release();
  }
  
  void DBReader::exit() {
    return;
  }

  DBReader::~DBReader() {
    delete _lock;
    delete _vals;
  }
      
  DBWriter::DBWriter(string path)
    : DBAccessor(path) {
    _lock = NULL;
    prev_umask = -1;
    enter();
  }

  DBWriter::~DBWriter() {
    exit();
    delete _lock;
    delete _vals;
  }

  void DBWriter::put(string key, string value) {
    _vals->insert({key,value});
  }
  
  void DBWriter::_delete(string key) {
    map<string, string>::iterator itr;
    itr = _vals->find(key);
    if(itr != _vals->end()) {
      _vals->erase(itr);
    }
  }
  
  void DBWriter::enter() {
    mkdirs_exists_ok(_path);
    prev_umask = umask(0);
    try {
      chmod(_path.c_str(), 0777);
      _lock = _get_lock(true);
      _vals = _read_values_locked();
    } catch(const exception& e) {
      umask(prev_umask);
      prev_umask = -1;
    }
  }

  void DBWriter::finally_outer() {
    umask(prev_umask);
    prev_umask = -1;
    _lock->release();
  }
  
  void DBWriter::exit() {
    _check_entered();
    string new_data_path = "NULL";
    string old_data_path = "NULL";
    string tempdir_path = "NULL";
    string data_path = "NULL";
    string file_path = "NULL";
    int tmp_fd;
    int result;
    char path[1024];
    std::ofstream file;
    
    // TODO Are files being fsynced correctly
    try { 
      result = snprintf(path, sizeof(path), "%s/.tmp_XXXXXX", _path.c_str());
      if (result < 0 ) throw OSError();
      tempdir_path = mkdtemp(path);
      chmod(tempdir_path.c_str(), 0777);
      map<string,string>::iterator itr;
      for (itr = _vals->begin(); itr != _vals->end(); itr++) {
        file_path = tempdir_path + "/" + itr->first;
        tmp_fd = open(file_path.c_str(), O_DIRECTORY|O_WRONLY|O_APPEND|O_CREAT, 0700);
        if (tmp_fd < 0) throw OSError();
        file.open(file_path.c_str());
        file << itr->second;
        file.close();
        fsync(tmp_fd);
      }
      fsync_dir(tempdir_path.c_str());
      data_path = _data_path();

      try {
        //TODO This path MAY be wrong (not joined with absolute basepath)
        char buf[1024];
        ssize_t len = readlink(data_path.c_str(), buf, sizeof(buf)-1);
        if (len != -1) {
          buf[len] = '\0';
        } else {
          throw OSError();
        }
        old_data_path = string(buf);
      } catch (const exception& e){old_data_path = "NULL";}

      new_data_path = tempdir_path + "/.link"; 
      result = symlink(basename(tempdir_path.c_str()), new_data_path.c_str());
      if (result < 0 ) throw OSError();
      rename(new_data_path.c_str(), data_path.c_str());
      fsync_dir(_path.c_str());
      } catch (const exception& e) {
        goto finally;
      }

      finally:
        char buf[1024];
        ssize_t len = readlink(data_path.c_str(), buf, sizeof(buf)-1);
        if (len != -1) {
          buf[len] = '\0';
        } else {
          throw OSError();
        }
        string sym_check = string(buf);
        bool success = new_data_path != "NULL" && exists(data_path)
          && basename(tempdir_path.c_str()) == sym_check;

        if (success) {
          if (old_data_path != "NULL") {
            remove_all(old_data_path);
          }
        } else {
          remove_all(tempdir_path);
        }

        if (new_data_path != "NULL" && is_symlink(new_data_path)) {
          remove(new_data_path.c_str());
        }
        finally_outer();
  }
  
  string Params::read_db(string params_path, string key) {
    string path = params_path + "/d/" + key;
    string ret_str = "";
    std::ifstream reader;
    std::stringstream buffer;
    try {
      reader.open(path);
      buffer << reader.rdbuf();
      ret_str = buffer.str();
      reader.close();
    } catch (const exception& e) {ret_str = "";}
    return ret_str;
  }

  void Params::write_db(string params_path, string key, string value) {
    int prev_umask = umask(0);
    FileLock *lock = new FileLock(params_path + "/.lock", true);
    lock->acquire();
    string data_path;
    string new_data_path;
    char path[1024];
    int tmp_fd;
    int result;
    std::ofstream file;

    // TODO Are files being fynced correctly

    try {
      result = snprintf(path, sizeof(path), "%s/.tmp_XXXXXX", params_path.c_str());
      if (result < 0 ) throw OSError();
      tmp_fd = mkstemp(path);
      if (tmp_fd < 0 ) throw OSError();
      file.open(path);
      file << value;
      file.close();
      fsync(tmp_fd);
      data_path = params_path + "/d/" + key;
      result = rename(path, data_path.c_str());
      if (result < 0 ) throw OSError();
      chmod(data_path.c_str(), 0666);

      fsync_dir((params_path + "/d").c_str());
    } catch (const exception& e) {goto finally;}
    
    finally:
      umask(prev_umask);
      lock->release();
      delete lock;
  }
      
  Params::Params(string d) {
    db = d;
  
    if (!exists(db + "/d")) {
      DBAccessor* accessor = transaction(true);
      delete accessor;
    }
  }

  void Params::clear_all() {
    remove_all(db);
    DBAccessor* accessor = transaction(true);
    delete accessor;
  }
   
  DBAccessor* Params::transaction(bool write) {
    if (write) {
      return new DBWriter(db);
    } else {
      return new DBReader(db);
    }
  }

  bool Params::has(vector<TxType> v, TxType t) {
    vector<TxType>::iterator itr;
    
    for (itr = v.begin(); itr != v.end(); itr++) {
      if (*itr == t) return true;  
    } 
    return false;
  }


  void Params::_clear_keys_with_type(TxType t) {
    DBAccessor* accessor = transaction(true);
    map<string,vector<TxType>>::iterator itr;

    for(itr = keys.begin(); itr != keys.end(); itr++) {
      if(has(itr->second, t)) {
        accessor->_delete(itr->first);
      }
    }
    delete accessor;
  }
   
  void Params::manager_start() {
    _clear_keys_with_type(TxType::CLEAR_ON_MANAGER_START);
  }

  void Params::panda_disconnect() {
    _clear_keys_with_type(TxType::CLEAR_ON_PANDA_DISCONNECT);
  }
   
  void Params::_delete(string key) {
    DBAccessor* accessor = transaction(true);
    accessor->_delete(key);
    delete accessor;
  }

  string Params::get(string key) {
    string val = "";
    try {
      keys.at(key);
    } catch(const exception&) {
      throw UnknownKeyName();
    }
    val = read_db(db, key);
    return val;
  }

  string Params::get(string key, bool block) {
    string val = "";
    try {
      keys.at(key);
    } catch(const exception&) {
      throw UnknownKeyName();
    }
    while (true) {
      val = read_db(db, key);
      if (!block || (val != "")) {
        break;
      }
      sleep(.05);
    }
    return val;
  }
   
  void Params::put(string key, string data) {
    try {
      keys.at(key);
    } catch(const exception&) {
      throw UnknownKeyName();
    }  
    write_db(db, key, data); 
  }

  void Params::f() {
    sleep(.1);
    put("CarParams", "test");
  }

}

using namespace params;

int main() {
 
  // Testing mkdirs
  string test_path = current_path() + "/test";
  
  mkdirs_exists_ok(test_path);

  // Successful fsync
  int result = fsync_dir(test_path.c_str());
  assert(result == 0);

  // Failed fsync
  test_path = current_path() + "/cat///";
  result = fsync_dir(test_path.c_str());
  assert(result == -1);
  test_path = current_path() + "/test";
  DBAccessor* accessor = new DBReader(test_path);

  //Entered DB.
  bool flag = true;
  try {
    accessor->_check_entered();
  } catch (const exception& e) {
    flag =false;
  }
  assert(flag);
  delete accessor;
  //DBWriter
  test_path = current_path() + "/test";
  accessor = new DBWriter(test_path);

  //Entered DB.
  flag = true;
  try {
    accessor->_check_entered();
  } catch (const exception& e) {
    flag = false;
  }
  assert(flag);

  //Check data path
  assert(accessor->_data_path() == test_path + "/d");
  
  //Check data path
  assert(accessor->_data_path() != test_path + "/f");
  
  assert(accessor->_data_path() == (test_path + "/d").c_str());

  // FileLock not created
  FileLock* f = accessor->_get_lock(false);
  assert(!exists((accessor->_data_path()) + "/.lock"));
  f->release();
  delete f;

  // FileLock created but hidden
  f = accessor->_get_lock(true);
  //assert(!fs::is_regular_file(fs::path(accessor->_data_path())/".lock"));
  f->release();
  delete f;

  delete accessor;  
  string tempdir_path;
  char path[1024];
  result = snprintf(path, sizeof(path), "%s/.tmp_XXXXXX", current_path().c_str());
  if (result < 0 ) throw OSError();
  tempdir_path = mkdtemp(path);

  // Testing Params

  Params p = Params(tempdir_path);
  p.put("PandaDongleId", "cat");
  assert(p.get("PandaDongleId") == "cat");

  // Test params no ascii
  p.put("CarParams", "\xe1\x90\xff");
  assert(p.get("CarParams") == "\xe1\x90\xff");

  //Test params get cleared panda disconnect.  
  p.put("CarParams", "test");
  p.put("DongleId", "cb38263377b873ee");
  assert(p.get("CarParams") == "test");
  p.panda_disconnect();
  assert(p.get("DongleId") != "");
  assert(p.get("CarParams") == "");

  //Test params cleared mananger start
  p.put("CarParams", "test");
  p.put("DongleId", "cb38263377b873ee");
  assert(p.get("CarParams") == "test");
  p.manager_start();
  assert(p.get("CarParams") == "");
  assert(p.get("DongleId") != "");
  p.clear_all();

  //Put/get two things
  p.put("DongleId", "bob");
  p.put("AthenadPid", "123");
  assert(p.get("DongleId") == "bob");
  assert(p.get("AthenadPid") == "123");

  flag = false;
  try {
    p.get("swag");
  } catch(const exception& e) {
    flag = true;
  }
  assert(flag);

  //Testing permissions
  p.put("DongleId", "test");
  string file_path = tempdir_path + "/d/" + "DongleId";
  assert(exists(file_path));
  mode_t perms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
  struct stat st; 
  stat(file_path.c_str(), &st);
  mode_t file_perm = st.st_mode;
  assert((file_perm & perms) == perms);


  // Testing blocking
  p.clear_all();
  p.put("DongleId", "test");
  p.get("DongleId", 1);



  // Testing blocking
  p.clear_all();
  std::thread t(&Params::f, &p);
  assert(p.get("CarParams") == "");
  assert(p.get("CarParams", true) == "test");


  t.detach();
  remove_all(tempdir_path);
  remove_all(test_path); 
  return 0;
}

