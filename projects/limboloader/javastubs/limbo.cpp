#include "baron/baron.h"
#include "limbo.h"

std::shared_ptr<FakeJni::JString> jnivm::com::playdead::limbo::LimboActivity::GetLimboCachedAssetsPath()
{
    auto context = std::make_shared<jnivm::android::content::Context>();
    return context->getCacheDir()->getPath();
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::playdead::limbo::LimboActivity)
{ FakeJni::Constructor<LimboActivity> {} },
// NOTE: the previous revision registered this under the name
// "GetLimboCachedAssetsPath" while the C++ method was spelled
// GetLimboCachedAassetsPath -- kept spelled consistently now so the lookup and
// the definition cannot drift apart again.
{ FakeJni::Function<&LimboActivity::GetLimboCachedAssetsPath> {}, "GetLimboCachedAssetsPath", FakeJni::JMethodID::PUBLIC },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::playdead::limbo::LimboAgeSignals)
{ FakeJni::Constructor<LimboAgeSignals> {} },
END_NATIVE_DESCRIPTOR
