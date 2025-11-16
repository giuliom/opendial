#include <gtest/gtest.h>
#include <alarm.h>

using namespace opendial::alarm;

class AlarmTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

TEST_F(AlarmTest, DefaultConstructor) {
    Alarm alarm;
    EXPECT_NE(alarm.uuid, "");
}


int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}