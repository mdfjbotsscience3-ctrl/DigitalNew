/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ContentParent.h"
#include "RegistryMessageUtils.h"
#include "nsResProtocolHandler.h"

#include "nsChromeRegistryChrome.h"

#if defined(XP_WIN)
#  include <windows.h>
#elif defined(XP_MACOSX)
#  include <CoreServices/CoreServices.h>
#endif

#include "nsArrayEnumerator.h"
#include "nsComponentManager.h"
#include "nsEnumeratorUtils.h"
#include "nsNetUtil.h"
#include "nsStringEnumerator.h"
#include "nsTextFormatter.h"
#include "nsXPCOMCIDInternal.h"

#include "mozilla/LookAndFeel.h"

#include "nsIObserverService.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/Components.h"
#include "mozilla/Preferences.h"
#include "nsIResProtocolHandler.h"
#include "nsIScriptError.h"
#include "nsIXULRuntime.h"

#define PACKAGE_OVERRIDE_BRANCH "chrome.override_package."

/**
 * MODIFICACIÓN PARA TU NAVEGADOR:
 * Cambiamos "classic/1.0" por tu identificador de skin personalizado.
 * Esto hará que el registro busque tus recursos de Azul y Celeste.
 */
#define SKIN "custom/azul-celeste"_ns

using namespace mozilla;
using mozilla::dom::ContentParent;
using mozilla::dom::PContentParent;
using mozilla::intl::LocaleService;

// We use a "best-fit" algorithm for matching locales and themes.
// 1) the exact selected locale/theme
// 2) (locales only) same language, different country
//    e.g. en-GB is the selected locale, only en-US is available
// 3) any available locale/theme

/**
 * Match the language-part of two lang-COUNTRY codes, hopefully but
 * not guaranteed to be in the form ab-CD or abz-CD. "ab" should also
 * work, any other garbage-in will produce undefined results as long
 * as it does not crash.
 */
static bool LanguagesMatch(const nsACString& a, const nsACString& b) {
  if (a.Length() < 2 || b.Length() < 2) return false;

  nsACString::const_iterator as, ae, bs, be;
  a.BeginReading(as);
  a.EndReading(ae);
  b.BeginReading(bs);
  b.EndReading(be);

  while (*as == *bs) {
    if (*as == '-') return true;

    ++as;
    ++bs;

    // reached the end
    if (as == ae && bs == be) return true;

    // "a" is short
    if (as == ae) return (*bs == '-');

    // "b" is short
    if (bs == be) return (*as == '-');
  }

  return false;
}

nsChromeRegistryChrome::nsChromeRegistryChrome()
    : mProfileLoaded(false), mDynamicRegistration(true) {}

nsChromeRegistryChrome::~nsChromeRegistryChrome() = default;

nsresult nsChromeRegistryChrome::Init() {
  nsresult rv = nsChromeRegistry::Init();
  if (NS_FAILED(rv)) return rv;

  bool safeMode = false;
  nsCOMPtr<nsIXULRuntime> xulrun(do_GetService(XULAPPINFO_SERVICE_CONTRACTID));
  if (xulrun) xulrun->GetInSafeMode(&safeMode);

  nsCOMPtr<nsIObserverService> obsService =
      mozilla::services::GetObserverService();
  if (obsService) {
    obsService->AddObserver(this, "profile-initial-state", true);
    obsService->AddObserver(this, "intl:app-locales-changed", true);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistryChrome::GetLocalesForPackage(
    const nsACString& aPackage, nsIUTF8StringEnumerator** aResult) {
  nsCString realpackage;
  nsresult rv = OverrideLocalePackage(aPackage, realpackage);
  if (NS_FAILED(rv)) return rv;

  nsTArray<nsCString>* a = new nsTArray<nsCString>;
  if (!a) return NS_ERROR_OUT_OF_MEMORY;

  PackageEntry* entry;
  if (mPackagesHash.Get(realpackage, &entry)) {
    entry->locales.EnumerateToArray(a);
  }

  rv = NS_NewAdoptingUTF8StringEnumerator(aResult, a);
  if (NS_FAILED(rv)) delete a;

  return rv;
}

NS_IMETHODIMP
nsChromeRegistryChrome::IsLocaleRTL(const nsACString& package, bool* aResult) {
  *aResult = false;

  nsAutoCString locale;
  GetSelectedLocale(package, locale);
  if (locale.Length() < 2) return NS_OK;

  *aResult = LocaleService::IsLocaleRTL(locale);
  return NS_OK;
}

/**
 * This method negotiates only between the app locale and the available
 * chrome packages.
 *
 * If you want to get the current application's UI locale, please use
 * LocaleService::GetAppLocaleAsBCP47.
 */
nsresult nsChromeRegistryChrome::GetSelectedLocale(const nsACString& aPackage,
                                                   nsACString& aLocale) {
  nsAutoCString reqLocale;
  if (aPackage.EqualsLiteral("global")) {
    LocaleService::GetInstance()->GetAppLocaleAsBCP47(reqLocale);
  } else {
    AutoTArray<nsCString, 10> requestedLocales;
    LocaleService::GetInstance()->GetRequestedLocales(requestedLocales);
    reqLocale.Assign(requestedLocales[0]);
  }

  nsCString realpackage;
  nsresult rv = OverrideLocalePackage(aPackage, realpackage);
  if (NS_FAILED(rv)) return rv;
  PackageEntry* entry;
  if (!mPackagesHash.Get(realpackage, &entry)) return NS_ERROR_FILE_NOT_FOUND;

  aLocale = entry->locales.GetSelected(reqLocale, nsProviderArray::LOCALE);
  if (aLocale.IsEmpty()) return NS_ERROR_FAILURE;

  return NS_OK;
}

nsresult nsChromeRegistryChrome::OverrideLocalePackage(
    const nsACString& aPackage, nsACString& aOverride) {
  const nsACString& pref = nsLiteralCString(PACKAGE_OVERRIDE_BRANCH) + aPackage;
  nsAutoCString override;
  nsresult rv = mozilla::Preferences::GetCString(PromiseFlatCString(pref).get(),
                                                  override);
  if (NS_SUCCEEDED(rv)) {
    aOverride = override;
  } else {
    aOverride = aPackage;
  }
  return NS_OK;
}

NS_IMETHODIMP
nsChromeRegistryChrome::Observe(nsISupports* aSubject, const char* aTopic,
                                const char16_t* someData) {
  nsresult rv = NS_OK;

  if (!strcmp("profile-initial-state", aTopic)) {
    mProfileLoaded = true;
  } else if (!strcmp("intl:app-locales-changed", aTopic)) {
    if (mProfileLoaded) {
      FlushAllCaches();
    }
  } else {
    NS_ERROR("Unexpected observer topic!");
  }

  return rv;
}

NS_IMETHODIMP
nsChromeRegistryChrome::CheckForNewChrome() {
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed)) {
    MOZ_ASSERT(false, "checking for new chrome during shutdown");
    return NS_ERROR_UNEXPECTED;
  }

  mPackagesHash.Clear();
  mOverrideTable.Clear();

  mDynamicRegistration = false;

  nsComponentManagerImpl::gComponentManager->RereadChromeManifests();

  mDynamicRegistration = true;

  SendRegisteredChrome(nullptr);
  return NS_OK;
}

static void SerializeURI(nsIURI* aURI, SerializedURI& aSerializedURI) {
  if (!aURI) return;

  aURI->GetSpec(aSerializedURI.spec);
}

void nsChromeRegistryChrome::SendRegisteredChrome(
    mozilla::dom::PContentParent* aParent) {
  nsTArray<ChromePackage> packages;
  nsTArray<SubstitutionMapping> resources;
  nsTArray<OverrideMapping> overrides;

  for (const auto& entry : mPackagesHash) {
    ChromePackage chromePackage;
    ChromePackageFromPackageEntry(entry.GetKey(), entry.GetWeak(),
                                  &chromePackage, SKIN);
    packages.AppendElement(chromePackage);
  }

  if (aParent) {
    nsCOMPtr<nsIIOService> io(do_GetIOService());
    NS_ENSURE_TRUE_VOID(io);

    nsCOMPtr<nsIProtocolHandler> ph;
    nsresult rv = io->GetProtocolHandler("resource", getter_AddRefs(ph));
    NS_ENSURE_SUCCESS_VOID(rv);

    nsCOMPtr<nsIResProtocolHandler> irph(do_QueryInterface(ph));
    nsResProtocolHandler* rph = static_cast<nsResProtocolHandler*>(irph.get());
    rv = rph->CollectSubstitutions(resources);
    NS_ENSURE_SUCCESS_VOID(rv);
  }

  for (const auto& entry : mOverrideTable) {
    SerializedURI chromeURI, overrideURI;

    SerializeURI(entry.GetKey(), chromeURI);
    SerializeURI(entry.GetWeak(), overrideURI);

    overrides.AppendElement(
        OverrideMapping{std::move(chromeURI), std::move(overrideURI)});
  }

  nsAutoCString appLocale;
  LocaleService::GetInstance()->GetAppLocaleAsBCP47(appLocale);

  if (aParent) {
    bool success = aParent->SendRegisterChrome(packages, resources, overrides,
                                               appLocale, false);
    NS_ENSURE_TRUE_VOID(success);
  } else {
    nsTArray<ContentParent*> parents;
    ContentParent::GetAll(parents);
    if (!parents.Length()) return;

    for (uint32_t i = 0; i < parents.Length(); i++) {
  
      /* © 2026 MDFJ Bots. Science - All Rights Reserved
       * © mdfjbots. Compatible con Google Chrome, Firefox, Safari y Edge
       * DigitalNew base Firefox modified by MDFJ Bots. Science/. */
