// Implement a KeyValue interface to a RocksDB on-disk database.
//
#include <cstddef>
#include <map>
#include "KeyValue.h"
#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"

std::string kDBPath = "/tmp/rocksdb_simple_example";

namespace GLnexus {
namespace RocksIntf {

    // Convert a rocksdb Status into a GLnexus Status structure
    static Status convertStatus(const rocksdb::Status &s)
    {
        rocksdb::Status::Code code = s.code();
        switch (code) {
            // good case
        case rocksdb::Status::kOk: return Status::OK();

            // error cases
        case rocksdb::Status::kNotFound: 
            return Status::NotFound();
        case rocksdb::Status::kCorruption: 
            return Status::Failure("corruption");
        case rocksdb::Status::kNotSupported: 
            return Status::NotImplemented();
        case rocksdb::Status::kInvalidArgument: 
            return Status::Invalid();
        case rocksdb::Status::kIOError: 
            return Status::IOError();
        case rocksdb::Status::kMergeInProgress: 
            return Status::Failure("merge in progress");
        case rocksdb::Status::kIncomplete: 
            return Status::Failure("incomplete");
        case rocksdb::Status::kShutdownInProgress: 
            return Status::Failure("shutdown in progress");
        case rocksdb::Status::kTimedOut: 
            return Status::Failure("timed out");
        case rocksdb::Status::kAborted: 
            return Status::Failure("aborted");

            // catch all for unlisted cases, all errors
        default: return Status::Failure("other reason");
        }
    }


    class Iterator : public KeyValue::Iterator {
    private:
        rocksdb::Iterator* iter_;

        // No copying allowed
        Iterator(const Iterator&);
        void operator=(const Iterator&);

    public:
        // Note: the NewIterator call allocates heap-memory
        Iterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* coll)
        {
            rocksdb::ReadOptions options;  // default values
            iter_ = db->NewIterator(options, coll);
            iter_->SeekToFirst();
        }

        // desctructor. We need to release the heap memory used by the
        // the iterator.
        ~Iterator() {
            delete iter_;
        }

        Status next(std::string& key, std::string& value) override {
            // It seems kind of silly to call Valid twice here. I am
            // not sure how to avoid doing this, while maintaining safety.
            //
            if (!iter_->Valid())
                return Status::NotFound();
            iter_->Next();
            if (!iter_->Valid())
                return Status::NotFound();
            key = iter_->key().ToString();
            value = iter_->value().ToString();
            return Status::OK();
        }

        Status seek(const std::string& key) {
            if (!iter_->Valid())
                return Status::NotFound();
            iter_->Seek(key);
            if (!iter_->Valid())
                return Status::NotFound();
            return Status::OK();
        }
    };


    class Reader : public KeyValue::Reader<rocksdb::ColumnFamilyHandle*,Iterator> {
    private:
        rocksdb::DB* db_ = NULL;

        // No copying allowed
        Reader(const Reader&);
        void operator=(const Reader&);
        
    public:
        Reader() {}
            
        Reader(rocksdb::DB *db) {
            db_ = db;
        }

        ~Reader() {}

        Status get(rocksdb::ColumnFamilyHandle* &coll,
                   const std::string& key,
                   std::string& value) const override {
            const rocksdb::ReadOptions r_options; // what should this be set to?
            std::string* v_tmp; // convert from pointer to reference, can we 
            rocksdb::Status s = db_->Get(r_options, coll, key, v_tmp);
            value = *v_tmp; 
            return convertStatus(s);
        }

        Status iterator(rocksdb::ColumnFamilyHandle* &coll,
                        std::unique_ptr<Iterator>& it) const override {
            it = std::make_unique<Iterator>(db_, coll);
            return Status::OK();
        }

        Status iterator(rocksdb::ColumnFamilyHandle* &coll,
                        const std::string& key,
                        std::unique_ptr<Iterator>& it) const override {
            it = std::make_unique<Iterator>(db_, coll);
            return it->seek(key);
        }
    };


    class WriteBatch : public KeyValue::WriteBatch<rocksdb::ColumnFamilyHandle*> {
    private:
        rocksdb::WriteBatch *wb_;
        friend class DB;

        // No copying allowed
        WriteBatch(const WriteBatch&);
        void operator=(const WriteBatch&);

    public:
        WriteBatch() {
            wb_ = new rocksdb::WriteBatch();
        }

        ~WriteBatch() {
            delete wb_;
        }

        rocksdb::WriteBatch* GetImplObj() {
            return wb_;
        }

        Status put(rocksdb::ColumnFamilyHandle* &coll,
                   const std::string& key,
                   const std::string& value) override {
            wb_->Put(coll, key, value);
            return Status::OK();
        }
    };


    class DB : public KeyValue::DB<rocksdb::ColumnFamilyHandle*, Reader, Iterator, WriteBatch> {
    private:
        rocksdb::DB* db_ = NULL;
        std::map<const std::string, rocksdb::ColumnFamilyHandle*> coll2handle_;

        // No copying allowed
        DB(const DB&);
        void operator=(const DB&);

    public:
        DB(const std::vector<std::string>& collections) {
            // Default optimization choices, could use a deeper look.
            rocksdb::Options options;
            options.IncreaseParallelism();
            options.OptimizeLevelStyleCompaction();
            options.create_if_missing = true;

            // open DB
            rocksdb::Status s = rocksdb::DB::Open(options, kDBPath, &db_);
            assert(s.ok());

            // create initial collections
            for (const auto &colName : collections) {
                rocksdb::ColumnFamilyOptions options; // defaults for now
                rocksdb::ColumnFamilyHandle *handle;
                rocksdb::Status s = db_->CreateColumnFamily(options, colName, &handle);
                assert(s.ok());
                
                // success, add a mapping from the column family name to the handle.
                coll2handle_[colName] = handle;
            }
        }

        ~DB() {
            delete db_;
        }

        Status collection(const std::string& name, 
                          rocksdb::ColumnFamilyHandle*& coll) const override {
            auto p = coll2handle_.find(name);
            if (p != coll2handle_.end()) {
                coll = p->second;
                return Status::OK();
            }
            return Status::NotFound("column family does not exist", name);
        }

        Status create_collection(const std::string& name) override {
            if (coll2handle_.find(name) != coll2handle_.end()) {
                return Status::Exists("column family already exists", name);
            }

            // create new column family in rocksdb
            rocksdb::ColumnFamilyOptions options; // defaults for now
            rocksdb::ColumnFamilyHandle *handle;
            rocksdb::Status s = db_->CreateColumnFamily(options, name, &handle);
            if (!s.ok())
                return convertStatus(s);

            // success, add a mapping from the column family name to the handle.
            coll2handle_[name] = handle;
            return Status::OK();
        }

        Status current(std::unique_ptr<Reader>& reader) const override {
            reader = std::make_unique<Reader>(db_);
            return Status::OK();
        }

        Status begin_writes(std::unique_ptr<WriteBatch>& writes) override {
            writes = std::make_unique<WriteBatch>();
            return Status::OK();
        }

        Status commit_writes(WriteBatch* updates) override {
            rocksdb::WriteOptions options;
            options.sync = true;
            rocksdb::Status s = db_->Write(options, updates->GetImplObj());
            return convertStatus(s);
        }

        void wipe() {
            // Not sure what the semantics are supposed to be here. 
            rocksdb::Options options;
            rocksdb::DestroyDB(kDBPath, options);
        }
    };
}}
