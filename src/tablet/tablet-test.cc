// Copyright (c) 2012, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <time.h>

#include "common/row.h"
#include "tablet/memstore.h"
#include "tablet/tablet.h"
#include "util/slice.h"
#include "util/test_macros.h"

namespace kudu {
namespace tablet {


class TestTablet : public ::testing::Test {
public:
  TestTablet() :
    ::testing::Test(),
    env_(Env::Default()),
    schema_(boost::assign::list_of
            (ColumnSchema("key", STRING))
            (ColumnSchema("val", UINT32)),
            1),
    arena_(1024, 4*1024*1024)
  {}
protected:

  virtual void SetUp() {
    const ::testing::TestInfo* const test_info =
      ::testing::UnitTest::GetInstance()->current_test_info();

    ASSERT_STATUS_OK(env_->GetTestDirectory(&test_dir_));

    test_dir_ += StringPrintf("/%s.%s.%ld",
                              test_info->test_case_name(),
                              test_info->name(),
                              time(NULL));

    LOG(INFO) << "Creating tablet in: " << test_dir_;
    tablet_.reset(new Tablet(schema_, test_dir_));
    ASSERT_STATUS_OK(tablet_->CreateNew());
    ASSERT_STATUS_OK(tablet_->Open());
  }

  Env *env_;
  const Schema schema_;
  string test_dir_;
  scoped_ptr<Tablet> tablet_;

  Arena arena_;
};

TEST_F(TestTablet, TestFlush) {
  // Insert 1000 rows into memstore
  RowBuilder rb(schema_);
  char buf[256];
  for (int i = 0; i < 1000; i++) {
    rb.Reset();
    snprintf(buf, sizeof(buf), "hello %d", i);
    rb.AddString(Slice(buf));

    rb.AddUint32(i);
    ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
  }

  // Flush it.
  ASSERT_STATUS_OK(tablet_->Flush());

  // TODO: assert that the data can still be read after the flush.
}

// Test that inserting a row which already exists causes an AlreadyPresent
// error
TEST_F(TestTablet, TestInsertDuplicateKey) {
  RowBuilder rb(schema_);
  rb.AddString(Slice("hello world"));
  rb.AddUint32(12345);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));

  // Insert again, should fail!
  Status s = tablet_->Insert(rb.data());
  ASSERT_TRUE(s.IsAlreadyPresent()) <<
    "expected AlreadyPresent, but got: " << s.ToString();

  // Flush, and make sure that inserting duplicate still fails
  ASSERT_STATUS_OK(tablet_->Flush());

  s = tablet_->Insert(rb.data());
  ASSERT_TRUE(s.IsAlreadyPresent()) <<
    "expected AlreadyPresent, but got: " << s.ToString();
}

// Test iterating over a whole tablet
TEST_F(TestTablet, TestRowIterator) {
  // Put a row in disk layer 1 (insert and flush)
  RowBuilder rb(schema_);
  rb.AddString(Slice("hello from layer 1"));
  rb.AddUint32(1);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
  ASSERT_STATUS_OK(tablet_->Flush());

  // Put a row in disk layer 2 (insert and flush)
  rb.Reset();
  rb.AddString(Slice("hello from layer 2"));
  rb.AddUint32(2);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));
  ASSERT_STATUS_OK(tablet_->Flush());

  // Put a row in memstore
  rb.Reset();
  rb.AddString(Slice("hello from memstore"));
  rb.AddUint32(3);
  ASSERT_STATUS_OK(tablet_->Insert(rb.data()));

  // Now iterate the tablet and make sure the rows show up
  scoped_ptr<Tablet::RowIterator> iter;
  ASSERT_STATUS_OK(tablet_->NewRowIterator(schema_, &iter));

  scoped_array<uint8_t> buf(new uint8_t[schema_.byte_size() * 100]);

  // First call to CopyNextRows should fetch the whole memstore.
  size_t n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &buf[0], &arena_));
  ASSERT_EQ(1, n) << "should get only the one row from memstore";
  ASSERT_EQ("(string key=hello from memstore, uint32 val=3)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from memstore";

  // Next, should fetch the older layer
  n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &buf[0], &arena_));
  ASSERT_EQ(1, n) << "should get only the one row from layer 1";
  ASSERT_EQ("(string key=hello from layer 1, uint32 val=1)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from layer 1";

  // Next, should fetch the newer layer
  n = 100;
  ASSERT_STATUS_OK(iter->CopyNextRows(&n, &buf[0], &arena_));
  ASSERT_EQ(1, n) << "should get only the one row from layer 2";
  ASSERT_EQ("(string key=hello from layer 2, uint32 val=2)",
            schema_.DebugRow(&buf[0]))
    << "should have retrieved the row data from layer 2";

  // TODO: ASSERT_FALSE(iter->HasNext());
}

}
}
