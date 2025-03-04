// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/translate/core/browser/translate_prefs.h"

#include <memory>
#include <set>
#include <utility>

#include "base/feature_list.h"
#include "base/i18n/rtl.h"
#include "base/strings/string16.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/language/core/common/language_experiments.h"
#include "components/language/core/common/locale_util.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/translate/core/browser/translate_accept_languages.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/common/translate_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_collator.h"

namespace translate {

const char kForceTriggerTranslateCount[] =
    "translate_force_trigger_on_english_count";
const char TranslatePrefs::kPrefTranslateSiteBlacklist[] =
    "translate_site_blacklist";
const char TranslatePrefs::kPrefTranslateWhitelists[] = "translate_whitelists";
const char TranslatePrefs::kPrefTranslateDeniedCount[] =
    "translate_denied_count_for_language";
const char TranslatePrefs::kPrefTranslateIgnoredCount[] =
    "translate_ignored_count_for_language";
const char TranslatePrefs::kPrefTranslateAcceptedCount[] =
    "translate_accepted_count";
const char TranslatePrefs::kPrefTranslateBlockedLanguages[] =
    "translate_blocked_languages";
const char TranslatePrefs::kPrefTranslateLastDeniedTimeForLanguage[] =
    "translate_last_denied_time_for_language";
const char TranslatePrefs::kPrefTranslateTooOftenDeniedForLanguage[] =
    "translate_too_often_denied_for_language";
const char TranslatePrefs::kPrefTranslateRecentTarget[] =
    "translate_recent_target";

#if defined(OS_ANDROID)
const char TranslatePrefs::kPrefTranslateAutoAlwaysCount[] =
    "translate_auto_always_count";
const char TranslatePrefs::kPrefTranslateAutoNeverCount[] =
    "translate_auto_never_count";
#endif

// The below properties used to be used but now are deprecated. Don't use them
// since an old profile might have some values there.
//
// * translate_last_denied_time
// * translate_too_often_denied
// * translate_language_blacklist

namespace {

// Expands language codes to make these more suitable for Accept-Language.
// Example: ['en-US', 'ja', 'en-CA'] => ['en-US', 'en', 'ja', 'en-CA'].
// 'en' won't appear twice as this function eliminates duplicates.
// The StringPieces in |expanded_languages| are references to the strings in
// |languages|.
void ExpandLanguageCodes(const std::vector<std::string>& languages,
                         std::vector<base::StringPiece>* expanded_languages) {
  DCHECK(expanded_languages);
  DCHECK(expanded_languages->empty());

  // used to eliminate duplicates.
  std::set<base::StringPiece> seen;

  for (const auto& language : languages) {
    if (seen.find(language) == seen.end()) {
      expanded_languages->push_back(language);
      seen.insert(language);
    }

    std::vector<base::StringPiece> tokens = base::SplitStringPiece(
        language, "-", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    if (tokens.size() == 0)
      continue;

    base::StringPiece main_part = tokens[0];
    if (seen.find(main_part) == seen.end()) {
      expanded_languages->push_back(main_part);
      seen.insert(main_part);
    }
  }
}

}  // namespace

const base::Feature kImprovedLanguageSettings{"ImprovedLanguageSettings",
                                              base::FEATURE_ENABLED_BY_DEFAULT};

const base::Feature kRegionalLocalesAsDisplayUI{
    "RegionalLocalesAsDisplayUI", base::FEATURE_ENABLED_BY_DEFAULT};

const base::Feature kTranslateRecentTarget{"TranslateRecentTarget",
                                           base::FEATURE_ENABLED_BY_DEFAULT};

DenialTimeUpdate::DenialTimeUpdate(PrefService* prefs,
                                   const std::string& language,
                                   size_t max_denial_count)
    : denial_time_dict_update_(
          prefs,
          TranslatePrefs::kPrefTranslateLastDeniedTimeForLanguage),
      language_(language),
      max_denial_count_(max_denial_count),
      time_list_(nullptr) {}

DenialTimeUpdate::~DenialTimeUpdate() {}

// Gets the list of timestamps when translation was denied.
base::ListValue* DenialTimeUpdate::GetDenialTimes() {
  if (time_list_)
    return time_list_;

  // Any consumer of GetDenialTimes _will_ write to them, so let's get an
  // update started.
  base::DictionaryValue* denial_time_dict = denial_time_dict_update_.Get();
  DCHECK(denial_time_dict);

  base::Value* denial_value = nullptr;
  bool has_value = denial_time_dict->Get(language_, &denial_value);
  bool has_list = has_value && denial_value->GetAsList(&time_list_);

  if (!has_list) {
    auto time_list = std::make_unique<base::ListValue>();
    double oldest_denial_time = 0;
    bool has_old_style =
        has_value && denial_value->GetAsDouble(&oldest_denial_time);
    if (has_old_style)
      time_list->AppendDouble(oldest_denial_time);
    time_list_ = denial_time_dict->SetList(language_, std::move(time_list));
  }
  return time_list_;
}

base::Time DenialTimeUpdate::GetOldestDenialTime() {
  double oldest_time;
  bool result = GetDenialTimes()->GetDouble(0, &oldest_time);
  if (!result)
    return base::Time();
  return base::Time::FromJsTime(oldest_time);
}

void DenialTimeUpdate::AddDenialTime(base::Time denial_time) {
  DCHECK(GetDenialTimes());
  GetDenialTimes()->AppendDouble(denial_time.ToJsTime());

  while (GetDenialTimes()->GetSize() >= max_denial_count_)
    GetDenialTimes()->Remove(0, nullptr);
}

TranslateLanguageInfo::TranslateLanguageInfo() = default;

TranslateLanguageInfo::TranslateLanguageInfo(
    const TranslateLanguageInfo& other) = default;

TranslatePrefs::TranslatePrefs(PrefService* user_prefs,
                               const char* accept_languages_pref,
                               const char* preferred_languages_pref)
    : accept_languages_pref_(accept_languages_pref), prefs_(user_prefs) {
#if defined(OS_CHROMEOS)
  preferred_languages_pref_ = preferred_languages_pref;
#else
  DCHECK(!preferred_languages_pref);
#endif
}

bool TranslatePrefs::IsOfferTranslateEnabled() const {
  return prefs_->GetBoolean(prefs::kOfferTranslateEnabled);
}

bool TranslatePrefs::IsTranslateAllowedByPolicy() const {
  const PrefService::Preference* const pref =
      prefs_->FindPreference(prefs::kOfferTranslateEnabled);
  DCHECK(pref);
  DCHECK(pref->GetValue()->is_bool());

  return pref->GetValue()->GetBool() || !pref->IsManaged();
}

void TranslatePrefs::SetCountry(const std::string& country) {
  country_ = country;
}

std::string TranslatePrefs::GetCountry() const {
  return country_;
}

void TranslatePrefs::ResetToDefaults() {
  ClearBlockedLanguages();
  ClearBlacklistedSites();
  ClearWhitelistedLanguagePairs();
  prefs_->ClearPref(kPrefTranslateDeniedCount);
  prefs_->ClearPref(kPrefTranslateIgnoredCount);
  prefs_->ClearPref(kPrefTranslateAcceptedCount);

#if defined(OS_ANDROID)
  prefs_->ClearPref(kPrefTranslateAutoAlwaysCount);
  prefs_->ClearPref(kPrefTranslateAutoNeverCount);
#endif

  prefs_->ClearPref(kPrefTranslateLastDeniedTimeForLanguage);
  prefs_->ClearPref(kPrefTranslateTooOftenDeniedForLanguage);
}

bool TranslatePrefs::IsBlockedLanguage(
    const std::string& original_language) const {
  return IsValueBlacklisted(kPrefTranslateBlockedLanguages, original_language);
}

// Note: the language codes used in the language settings list have the Chrome
// internal format and not the Translate server format.
// To convert from one to the other use util functions
// ToTranslateLanguageSynonym() and ToChromeLanguageSynonym().
void TranslatePrefs::AddToLanguageList(const std::string& input_language,
                                       const bool force_blocked) {
  DCHECK(!input_language.empty());

  std::string chrome_language = input_language;
  translate::ToChromeLanguageSynonym(&chrome_language);

  std::vector<std::string> languages;
  GetLanguageList(&languages);

  // We should block the language if the list does not already contain another
  // language with the same base language.
  const bool should_block =
      !base::FeatureList::IsEnabled(kImprovedLanguageSettings) ||
      !language::ContainsSameBaseLanguage(languages, chrome_language);

  if (force_blocked || should_block) {
    BlockLanguage(input_language);
  }

  // Add the language to the list.
  if (std::find(languages.begin(), languages.end(), chrome_language) ==
      languages.end()) {
    languages.push_back(chrome_language);
    UpdateLanguageList(languages);
  }
}

void TranslatePrefs::RemoveFromLanguageList(const std::string& input_language) {
  DCHECK(!input_language.empty());

  std::string chrome_language = input_language;
  translate::ToChromeLanguageSynonym(&chrome_language);

  std::vector<std::string> languages;
  GetLanguageList(&languages);

  // Remove the language from the list.
  const auto& it =
      std::find(languages.begin(), languages.end(), chrome_language);
  if (it != languages.end()) {
    languages.erase(it);
    UpdateLanguageList(languages);

    if (base::FeatureList::IsEnabled(kImprovedLanguageSettings)) {
      // We should unblock the language if this was the last one from the same
      // language family.
      if (!language::ContainsSameBaseLanguage(languages, chrome_language)) {
        UnblockLanguage(input_language);
      }
    }
  }
}

void TranslatePrefs::RearrangeLanguage(
    const std::string& language,
    const TranslatePrefs::RearrangeSpecifier where,
    const int offset,
    const std::vector<std::string>& enabled_languages) {
  // Negative offset is not supported.
  DCHECK(!(offset < 1 && (where == kUp || where == kDown)));

  std::vector<std::string> languages;
  GetLanguageList(&languages);

  const std::vector<std::string>::iterator pos =
      std::find(std::begin(languages), std::end(languages), language);
  const int original_position = pos - languages.begin();
  const int length = languages.size();

  if (pos == std::end(languages)) {
    return;
  }

  // Create a set of enabled languages for fast lookup.
  const std::set<std::string> enabled(enabled_languages.begin(),
                                      enabled_languages.end());
  if (enabled.find(language) == enabled.end()) {
    return;
  }

  // |a| and |b| indicate the first and last position that we want to
  // rotate to the right. |r| is the position that we want to rotate to the
  // first position.
  int a, r, b;

  // In this block we need to skip languages that are not enabled, unless we're
  // moving to the top of the list.
  switch (where) {
    case kUp:
      a = original_position;
      r = original_position;
      b = original_position + 1;
      for (int steps = offset; steps > 0; --steps) {
        --a;
        while (a >= 0 && enabled.find(languages[a]) == enabled.end()) {
          --a;
        }
      }
      // Skip ahead of any non-enabled language that may be before the new
      // destination.
      {
        int prev = a - 1;
        while (prev >= 0 && enabled.find(languages[prev]) == enabled.end()) {
          --a;
          --prev;
        }
      }
      break;

    case kDown:
      a = original_position;
      r = original_position + 1;
      b = original_position;
      for (int steps = offset; steps > 0; --steps) {
        ++b;
        while (b < length && enabled.find(languages[b]) == enabled.end()) {
          ++b;
        }
      }
      ++b;
      break;

    case kTop:
      if (original_position <= 0) {
        return;
      }
      a = 0;
      r = original_position;
      b = r + 1;
      break;

    case kNone:
      return;

    default:
      NOTREACHED();
      return;
  }

  // Sanity checks before performing the rotation.
  a = std::max(0, a);
  b = std::min(length, b);
  if (r > a && r < b) {
    // All cases can be achieved with a single rotation.
    std::vector<std::string>::iterator first = languages.begin() + a;
    std::vector<std::string>::iterator it = languages.begin() + r;
    std::vector<std::string>::iterator last = languages.begin() + b;
    std::rotate(first, it, last);

    UpdateLanguageList(languages);
  }
}

// static
void TranslatePrefs::GetLanguageInfoList(
    const std::string& app_locale,
    bool translate_allowed,
    std::vector<TranslateLanguageInfo>* language_list) {
  DCHECK(language_list != nullptr);

  if (app_locale.empty()) {
    return;
  }

  language_list->clear();

  // Collect the language codes from the supported accept-languages.
  std::vector<std::string> language_codes;
  l10n_util::GetAcceptLanguagesForLocale(app_locale, &language_codes);

  // Map of [display name -> {language code, native display name}].
  typedef std::pair<std::string, base::string16> LanguagePair;
  typedef std::map<base::string16, LanguagePair,
                   l10n_util::StringComparator<base::string16>>
      LanguageMap;

  // Collator used to sort display names in the given locale.
  UErrorCode error = U_ZERO_ERROR;
  std::unique_ptr<icu::Collator> collator(
      icu::Collator::createInstance(icu::Locale(app_locale.c_str()), error));
  if (U_FAILURE(error)) {
    collator.reset();
  }
  LanguageMap language_map(
      l10n_util::StringComparator<base::string16>(collator.get()));

  // Build the list of display names and the language map.
  for (const auto& code : language_codes) {
    const base::string16 display_name =
        l10n_util::GetDisplayNameForLocale(code, app_locale, false);
    const base::string16 native_display_name =
        l10n_util::GetDisplayNameForLocale(code, code, false);
    language_map[display_name] = std::make_pair(code, native_display_name);
  }

  // Get the list of translatable languages and convert to a set.
  std::vector<std::string> translate_languages;
  translate::TranslateDownloadManager::GetSupportedLanguages(
      translate_allowed, &translate_languages);
  const std::set<std::string> translate_language_set(
      translate_languages.begin(), translate_languages.end());

  // Build the language list from the language map.
  for (const auto& entry : language_map) {
    const base::string16& display_name = entry.first;
    const LanguagePair& pair = entry.second;

    TranslateLanguageInfo language;
    language.code = pair.first;

    base::string16 adjusted_display_name(display_name);
    base::i18n::AdjustStringForLocaleDirection(&adjusted_display_name);
    language.display_name = base::UTF16ToUTF8(adjusted_display_name);

    base::string16 adjusted_native_display_name(pair.second);
    base::i18n::AdjustStringForLocaleDirection(&adjusted_native_display_name);
    language.native_display_name =
        base::UTF16ToUTF8(adjusted_native_display_name);

    std::string supports_translate_code = pair.first;
    if (base::FeatureList::IsEnabled(translate::kImprovedLanguageSettings)) {
      // Extract the base language: if the base language can be translated, then
      // even the regional one should be marked as such.
      translate::ToTranslateLanguageSynonym(&supports_translate_code);
    }
    language.supports_translate =
        translate_language_set.count(supports_translate_code) > 0;

    language_list->push_back(language);
  }
}

void TranslatePrefs::BlockLanguage(const std::string& input_language) {
  DCHECK(!input_language.empty());

  std::string translate_language = input_language;
  translate::ToTranslateLanguageSynonym(&translate_language);

  BlacklistValue(kPrefTranslateBlockedLanguages, translate_language);
}

void TranslatePrefs::UnblockLanguage(const std::string& input_language) {
  DCHECK(!input_language.empty());

  std::string translate_language = input_language;
  translate::ToTranslateLanguageSynonym(&translate_language);

  RemoveValueFromBlacklist(kPrefTranslateBlockedLanguages, translate_language);
}

bool TranslatePrefs::IsSiteBlacklisted(const std::string& site) const {
  return IsValueBlacklisted(kPrefTranslateSiteBlacklist, site);
}

void TranslatePrefs::BlacklistSite(const std::string& site) {
  DCHECK(!site.empty());
  BlacklistValue(kPrefTranslateSiteBlacklist, site);
}

void TranslatePrefs::RemoveSiteFromBlacklist(const std::string& site) {
  DCHECK(!site.empty());
  RemoveValueFromBlacklist(kPrefTranslateSiteBlacklist, site);
}

bool TranslatePrefs::IsLanguagePairWhitelisted(
    const std::string& original_language,
    const std::string& target_language) {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateWhitelists);
  if (dict && !dict->empty()) {
    std::string auto_target_lang;
    if (dict->GetString(original_language, &auto_target_lang) &&
        auto_target_lang == target_language)
      return true;
  }
  return false;
}

void TranslatePrefs::WhitelistLanguagePair(const std::string& original_language,
                                           const std::string& target_language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateWhitelists);
  base::DictionaryValue* dict = update.Get();
  if (!dict) {
    NOTREACHED() << "Unregistered translate whitelist pref";
    return;
  }
  dict->SetString(original_language, target_language);
}

void TranslatePrefs::RemoveLanguagePairFromWhitelist(
    const std::string& original_language,
    const std::string& target_language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateWhitelists);
  base::DictionaryValue* dict = update.Get();
  if (!dict) {
    NOTREACHED() << "Unregistered translate whitelist pref";
    return;
  }
  dict->Remove(original_language, nullptr);
}

bool TranslatePrefs::HasBlockedLanguages() const {
  return !IsListEmpty(kPrefTranslateBlockedLanguages);
}

void TranslatePrefs::ClearBlockedLanguages() {
  prefs_->ClearPref(kPrefTranslateBlockedLanguages);
}

bool TranslatePrefs::HasBlacklistedSites() const {
  return !IsListEmpty(kPrefTranslateSiteBlacklist);
}

void TranslatePrefs::ClearBlacklistedSites() {
  prefs_->ClearPref(kPrefTranslateSiteBlacklist);
}

bool TranslatePrefs::HasWhitelistedLanguagePairs() const {
  return !IsDictionaryEmpty(kPrefTranslateWhitelists);
}

void TranslatePrefs::ClearWhitelistedLanguagePairs() {
  prefs_->ClearPref(kPrefTranslateWhitelists);
}

int TranslatePrefs::GetTranslationDeniedCount(
    const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateDeniedCount);
  int count = 0;
  return dict->GetInteger(language, &count) ? count : 0;
}

void TranslatePrefs::IncrementTranslationDeniedCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateDeniedCount);
  base::DictionaryValue* dict = update.Get();

  int count = 0;
  dict->GetInteger(language, &count);
  dict->SetInteger(language, count + 1);
}

void TranslatePrefs::ResetTranslationDeniedCount(const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateDeniedCount);
  update.Get()->SetInteger(language, 0);
}

int TranslatePrefs::GetTranslationIgnoredCount(
    const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateIgnoredCount);
  int count = 0;
  return dict->GetInteger(language, &count) ? count : 0;
}

void TranslatePrefs::IncrementTranslationIgnoredCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateIgnoredCount);
  base::DictionaryValue* dict = update.Get();

  int count = 0;
  dict->GetInteger(language, &count);
  dict->SetInteger(language, count + 1);
}

void TranslatePrefs::ResetTranslationIgnoredCount(const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateIgnoredCount);
  update.Get()->SetInteger(language, 0);
}

int TranslatePrefs::GetTranslationAcceptedCount(
    const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateAcceptedCount);
  int count = 0;
  return dict->GetInteger(language, &count) ? count : 0;
}

void TranslatePrefs::IncrementTranslationAcceptedCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAcceptedCount);
  base::DictionaryValue* dict = update.Get();
  int count = 0;
  dict->GetInteger(language, &count);
  dict->SetInteger(language, count + 1);
}

void TranslatePrefs::ResetTranslationAcceptedCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAcceptedCount);
  update.Get()->SetInteger(language, 0);
}

#if defined(OS_ANDROID)
int TranslatePrefs::GetTranslationAutoAlwaysCount(
    const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateAutoAlwaysCount);
  int count = 0;
  return dict->GetInteger(language, &count) ? count : 0;
}

void TranslatePrefs::IncrementTranslationAutoAlwaysCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAutoAlwaysCount);
  base::DictionaryValue* dict = update.Get();
  int count = 0;
  dict->GetInteger(language, &count);
  dict->SetInteger(language, count + 1);
}

void TranslatePrefs::ResetTranslationAutoAlwaysCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAutoAlwaysCount);
  update.Get()->SetInteger(language, 0);
}

int TranslatePrefs::GetTranslationAutoNeverCount(
    const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateAutoNeverCount);
  int count = 0;
  return dict->GetInteger(language, &count) ? count : 0;
}

void TranslatePrefs::IncrementTranslationAutoNeverCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAutoNeverCount);
  base::DictionaryValue* dict = update.Get();
  int count = 0;
  dict->GetInteger(language, &count);
  dict->SetInteger(language, count + 1);
}

void TranslatePrefs::ResetTranslationAutoNeverCount(
    const std::string& language) {
  DictionaryPrefUpdate update(prefs_, kPrefTranslateAutoNeverCount);
  update.Get()->SetInteger(language, 0);
}
#endif  // defined(OS_ANDROID)

void TranslatePrefs::UpdateLastDeniedTime(const std::string& language) {
  if (IsTooOftenDenied(language))
    return;

  DenialTimeUpdate update(prefs_, language, 2);
  base::Time now = base::Time::Now();
  base::Time oldest_denial_time = update.GetOldestDenialTime();
  update.AddDenialTime(now);

  if (oldest_denial_time.is_null())
    return;

  if (now - oldest_denial_time <= base::TimeDelta::FromDays(1)) {
    DictionaryPrefUpdate update(prefs_,
                                kPrefTranslateTooOftenDeniedForLanguage);
    update.Get()->SetBoolean(language, true);
  }
}

bool TranslatePrefs::IsTooOftenDenied(const std::string& language) const {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateTooOftenDeniedForLanguage);
  bool result = false;
  return dict->GetBoolean(language, &result) ? result : false;
}

void TranslatePrefs::ResetDenialState() {
  prefs_->ClearPref(kPrefTranslateLastDeniedTimeForLanguage);
  prefs_->ClearPref(kPrefTranslateTooOftenDeniedForLanguage);
}

void TranslatePrefs::GetLanguageList(
    std::vector<std::string>* const languages) const {
  DCHECK(languages);
  DCHECK(languages->empty());

#if defined(OS_CHROMEOS)
  const char* key = preferred_languages_pref_.c_str();
#else
  const char* key = accept_languages_pref_.c_str();
#endif

  *languages = base::SplitString(prefs_->GetString(key), ",",
                                 base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
}

void TranslatePrefs::UpdateLanguageList(
    const std::vector<std::string>& languages) {
  std::string languages_str = base::JoinString(languages, ",");

#if defined(OS_CHROMEOS)
  prefs_->SetString(preferred_languages_pref_, languages_str);
#endif

  // Save the same language list as accept languages preference as well, but we
  // need to expand the language list, to make it more acceptable. For instance,
  // some web sites don't understand 'en-US' but 'en'. See crosbug.com/9884.
  if (!base::FeatureList::IsEnabled(kImprovedLanguageSettings)) {
    std::vector<base::StringPiece> accept_languages;
    ExpandLanguageCodes(languages, &accept_languages);
    languages_str = base::JoinString(accept_languages, ",");
  }
  prefs_->SetString(accept_languages_pref_, languages_str);
}

bool TranslatePrefs::CanTranslateLanguage(
    TranslateAcceptLanguages* accept_languages,
    const std::string& language) {
  bool can_be_accept_language =
      TranslateAcceptLanguages::CanBeAcceptLanguage(language);
  bool is_accept_language = accept_languages->IsAcceptLanguage(language);

  // Don't translate any user black-listed languages. Checking
  // |is_accept_language| is necessary because if the user eliminates the
  // language from the preference, it is natural to forget whether or not
  // the language should be translated. Checking |cannot_be_accept_language|
  // is also necessary because some minor languages can't be selected in the
  // language preference even though the language is available in Translate
  // server.
  return !IsBlockedLanguage(language) ||
         (!is_accept_language && can_be_accept_language);
}

bool TranslatePrefs::ShouldAutoTranslate(const std::string& original_language,
                                         std::string* target_language) {
  const base::DictionaryValue* dict =
      prefs_->GetDictionary(kPrefTranslateWhitelists);
  if (dict && dict->GetString(original_language, target_language)) {
    DCHECK(!target_language->empty());
    return !target_language->empty();
  }
  return false;
}

void TranslatePrefs::SetRecentTargetLanguage(
    const std::string& target_language) {
  prefs_->SetString(kPrefTranslateRecentTarget, target_language);
}

std::string TranslatePrefs::GetRecentTargetLanguage() const {
  return prefs_->GetString(kPrefTranslateRecentTarget);
}

int TranslatePrefs::GetForceTriggerOnEnglishPagesCount() const {
  return prefs_->GetInteger(kForceTriggerTranslateCount);
}

void TranslatePrefs::ReportForceTriggerOnEnglishPages() {
  int current_count = GetForceTriggerOnEnglishPagesCount();
  prefs_->SetInteger(kForceTriggerTranslateCount, current_count + 1);
}

void TranslatePrefs::ReportAcceptedAfterForceTriggerOnEnglishPages() {
  int current_count = GetForceTriggerOnEnglishPagesCount();
  if (current_count > 0)
    prefs_->SetInteger(kForceTriggerTranslateCount, current_count - 1);
}

// static
void TranslatePrefs::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterListPref(kPrefTranslateSiteBlacklist,
                             user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(
      kPrefTranslateWhitelists,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(
      kPrefTranslateDeniedCount,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(
      kPrefTranslateIgnoredCount,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(
      kPrefTranslateAcceptedCount,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterListPref(kPrefTranslateBlockedLanguages,
                             user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(kPrefTranslateLastDeniedTimeForLanguage);
  registry->RegisterDictionaryPref(
      kPrefTranslateTooOftenDeniedForLanguage,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterStringPref(kPrefTranslateRecentTarget, "",
                               user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterIntegerPref(
      kForceTriggerTranslateCount, 0,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);

#if defined(OS_ANDROID)
  registry->RegisterDictionaryPref(
      kPrefTranslateAutoAlwaysCount,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
  registry->RegisterDictionaryPref(
      kPrefTranslateAutoNeverCount,
      user_prefs::PrefRegistrySyncable::SYNCABLE_PREF);
#endif
}

// static
void TranslatePrefs::MigrateUserPrefs(PrefService* user_prefs,
                                      const char* accept_languages_pref) {
  // Old format of kPrefTranslateWhitelists
  // - original language -> list of target langs to auto-translate
  // - list of langs is in order of being enabled i.e. last in list is the
  //   most recent language that user enabled via
  //   Always translate |source_lang| to |target_lang|"
  // - this results in a one-to-n relationship between source lang and target
  //   langs.
  // New format:
  // - original language -> one target language to auto-translate
  // - each time that the user enables the "Always translate..." option, that
  //   target lang overwrites the previous one.
  // - this results in a one-to-one relationship between source lang and target
  //   lang
  // - we replace old list of target langs with the last target lang in list,
  //   assuming the last (i.e. most recent) target lang is what user wants to
  //   keep auto-translated.
  DictionaryPrefUpdate update(user_prefs, kPrefTranslateWhitelists);
  base::DictionaryValue* dict = update.Get();
  if (dict && !dict->empty()) {
    base::DictionaryValue::Iterator iter(*dict);
    while (!iter.IsAtEnd()) {
      const base::ListValue* list = nullptr;
      if (!iter.value().GetAsList(&list) || !list)
        break;  // Dictionary has either been migrated or new format.
      std::string key = iter.key();
      // Advance the iterator before removing the current element.
      iter.Advance();
      std::string target_lang;
      if (list->empty() ||
          !list->GetString(list->GetSize() - 1, &target_lang) ||
          target_lang.empty()) {
        dict->Remove(key, nullptr);
      } else {
        dict->SetString(key, target_lang);
      }
    }
  }
}

bool TranslatePrefs::IsValueInList(const base::ListValue* list,
                                   const std::string& in_value) const {
  for (size_t i = 0; i < list->GetSize(); ++i) {
    std::string value;
    if (list->GetString(i, &value) && value == in_value)
      return true;
  }
  return false;
}

bool TranslatePrefs::IsValueBlacklisted(const char* pref_id,
                                        const std::string& value) const {
  const base::ListValue* blacklist = prefs_->GetList(pref_id);
  return (blacklist && !blacklist->empty() && IsValueInList(blacklist, value));
}

void TranslatePrefs::BlacklistValue(const char* pref_id,
                                    const std::string& value) {
  ListPrefUpdate update(prefs_, pref_id);
  base::ListValue* blacklist = update.Get();
  if (!blacklist) {
    NOTREACHED() << "Unregistered translate blacklist pref";
    return;
  }

  if (IsValueBlacklisted(pref_id, value)) {
    return;
  }
  blacklist->AppendString(value);
}

void TranslatePrefs::RemoveValueFromBlacklist(const char* pref_id,
                                              const std::string& value) {
  ListPrefUpdate update(prefs_, pref_id);
  base::ListValue* blacklist = update.Get();
  if (!blacklist) {
    NOTREACHED() << "Unregistered translate blacklist pref";
    return;
  }

  base::Value string_value(value);
  blacklist->Remove(string_value, nullptr);
}

bool TranslatePrefs::IsListEmpty(const char* pref_id) const {
  const base::ListValue* blacklist = prefs_->GetList(pref_id);
  return (blacklist == nullptr || blacklist->empty());
}

bool TranslatePrefs::IsDictionaryEmpty(const char* pref_id) const {
  const base::DictionaryValue* dict = prefs_->GetDictionary(pref_id);
  return (dict == nullptr || dict->empty());
}

}  // namespace translate
