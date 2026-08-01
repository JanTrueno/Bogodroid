#include "robtop.h"

bool jnivm::com::customRobTop::base::BaseRobTopActivity::gameServicesIsSignedIn()
{
    return false;
}

int jnivm::com::customRobTop::base::BaseRobTopActivity::getAPILevel()
{
    return 21; // matches android.os.Build$VERSION.SDK_INT in the shared javastubs
}

long jnivm::com::customRobTop::base::BaseRobTopActivity::uptimeMillis()
{
    return (long)jnivm::android::os::uptimeMillis();
}

float jnivm::com::customRobTop::base::BaseRobTopActivity::getDeviceRefreshRate()
{
    return 60.0f;
}

std::shared_ptr<FakeJni::JString> jnivm::com::customRobTop::base::BaseRobTopActivity::loadAndDecryptFileToString(std::shared_ptr<FakeJni::JString> file)
{
    (void)file;
    return nullptr; // "no such file": the game falls back to its own save path
}

void jnivm::com::customRobTop::base::BaseRobTopActivity::loadingFinished()
{
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::customRobTop::base::BaseRobTopActivity)
    { FakeJni::Constructor<BaseRobTopActivity> {} },
    { FakeJni::Function<&BaseRobTopActivity::gameServicesIsSignedIn> {}, "gameServicesIsSignedIn", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&BaseRobTopActivity::getAPILevel> {}, "getAPILevel", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&BaseRobTopActivity::uptimeMillis> {}, "uptimeMillis", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&BaseRobTopActivity::getDeviceRefreshRate> {}, "getDeviceRefreshRate", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&BaseRobTopActivity::loadAndDecryptFileToString> {}, "loadAndDecryptFileToString", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&BaseRobTopActivity::loadingFinished> {}, "loadingFinished", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR
