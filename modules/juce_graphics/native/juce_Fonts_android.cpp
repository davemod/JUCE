/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

Typeface::Ptr Font::getDefaultTypefaceForFont (const Font& font)
{
    Font f (font);
    f.setTypefaceName ([&]() -> String
                       {
                           const auto faceName = font.getTypefaceName();

                           if (faceName == Font::getDefaultSansSerifFontName())    return "Roboto";
                           if (faceName == Font::getDefaultSerifFontName())        return "Roboto";
                           if (faceName == Font::getDefaultMonospacedFontName())   return "Roboto";

                           return faceName;
                       }());

    return Typeface::createSystemTypefaceFor (f);
}

//==============================================================================
#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 STATICMETHOD (create,          "create",           "(Ljava/lang/String;I)Landroid/graphics/Typeface;") \
 STATICMETHOD (createFromFile,  "createFromFile",   "(Ljava/lang/String;)Landroid/graphics/Typeface;") \
 STATICMETHOD (createFromAsset, "createFromAsset",  "(Landroid/content/res/AssetManager;Ljava/lang/String;)Landroid/graphics/Typeface;")

 DECLARE_JNI_CLASS (TypefaceClass, "android/graphics/Typeface")
#undef JNI_CLASS_MEMBERS

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 METHOD (constructor,          "<init>",           "()V") \
 METHOD (computeBounds,        "computeBounds",     "(Landroid/graphics/RectF;Z)V")

 DECLARE_JNI_CLASS (AndroidPath, "android/graphics/Path")
#undef JNI_CLASS_MEMBERS

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 METHOD (constructor,   "<init>",   "()V") \
 FIELD  (left,           "left",     "F") \
 FIELD  (right,          "right",    "F") \
 FIELD  (top,            "top",      "F") \
 FIELD  (bottom,         "bottom",   "F") \
 METHOD (roundOut,       "roundOut", "(Landroid/graphics/Rect;)V")

DECLARE_JNI_CLASS (AndroidRectF, "android/graphics/RectF")
#undef JNI_CLASS_MEMBERS

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 STATICMETHOD (getInstance, "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;") \
 METHOD       (update,      "update",      "([B)V") \
 METHOD       (digest,      "digest",      "()[B")
DECLARE_JNI_CLASS (JavaMessageDigest, "java/security/MessageDigest")
#undef JNI_CLASS_MEMBERS

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 METHOD       (open,      "open",      "(Ljava/lang/String;)Ljava/io/InputStream;") \

DECLARE_JNI_CLASS (AndroidAssetManager, "android/content/res/AssetManager")
#undef JNI_CLASS_MEMBERS

// Defined in juce_core
std::unique_ptr<InputStream> makeAndroidInputStreamWrapper (jobject stream);

//==============================================================================
class MemoryFontCache : public DeletedAtShutdown
{
public:
    ~MemoryFontCache()
    {
        clearSingletonInstance();
    }

    struct Key
    {
        String name, style;
        auto tie() const { return std::tuple (name, style); }
        bool operator< (const Key& other) const { return tie() < other.tie(); }
        bool operator== (const Key& other) const { return tie() == other.tie(); }
    };

    void add (const Key& key, std::shared_ptr<hb_font_t> ptr)
    {
        const std::scoped_lock lock { mutex };
        cache.emplace (key, ptr);
    }

    void remove (const Key& p)
    {
        const std::scoped_lock lock { mutex };
        cache.erase (p);
    }

    std::set<String> getAllNames() const
    {
        const std::scoped_lock lock { mutex };
        std::set<String> result;

        for (const auto& item : cache)
            result.insert (item.first.name);

        return result;
    }

    std::set<String> getStylesForFamily (const String& family) const
    {
        const std::scoped_lock lock { mutex };

        const auto lower = std::lower_bound (cache.begin(), cache.end(), family, [] (const auto& a, const String& b)
        {
            return a.first.name < b;
        });
        const auto upper = std::upper_bound (cache.begin(), cache.end(), family, [] (const String& a, const auto& b)
        {
            return a < b.first.name;
        });

        std::set<String> result;

        for (const auto& item : makeRange (lower, upper))
            result.insert (item.first.style);

        return result;
    }

    std::shared_ptr<hb_font_t> find (const Key& key)
    {
        const std::scoped_lock lock { mutex };

        const auto iter = cache.find (key);

        if (iter != cache.end())
            return iter->second;

        return nullptr;
    }

    JUCE_DECLARE_SINGLETON (MemoryFontCache, true)

private:
    std::map<Key, std::shared_ptr<hb_font_t>> cache;
    mutable std::mutex mutex;
};

JUCE_IMPLEMENT_SINGLETON (MemoryFontCache)

StringArray Font::findAllTypefaceNames()
{
    auto results = [&]
    {
        if (auto* cache = MemoryFontCache::getInstance())
            return cache->getAllNames();

        return std::set<String>{};
    }();

    for (auto& f : File ("/system/fonts").findChildFiles (File::findFiles, false, "*.ttf"))
        results.insert (f.getFileNameWithoutExtension().upToLastOccurrenceOf ("-", false, false));

    StringArray s;

    for (const auto& family : results)
        s.add (family);

    return s;
}

StringArray Font::findAllTypefaceStyles (const String& family)
{
    auto results = [&]
    {
        if (auto* cache = MemoryFontCache::getInstance())
            return cache->getStylesForFamily (family);

        return std::set<String>{};
    }();

    for (auto& f : File ("/system/fonts").findChildFiles (File::findFiles, false, family + "-*.ttf"))
        results.insert (f.getFileNameWithoutExtension().fromLastOccurrenceOf ("-", false, false));

    StringArray s;

    for (const auto& style : results)
        s.add (style);

    return s;
}

//==============================================================================
class AndroidTypeface final : public Typeface
{
public:
    static Typeface::Ptr from (const Font& font)
    {
        if (auto* cache = MemoryFontCache::getInstance())
            if (auto result = cache->find ({ font.getTypefaceName(), font.getTypefaceStyle() }))
                return new AndroidTypeface (DoCache::no, result, font.getTypefaceName(), font.getTypefaceStyle());

        auto blob = getBlobForFont (font);
        auto face = FontStyleHelpers::getFaceForBlob ({ static_cast<const char*> (blob.getData()), blob.getSize() }, 0);

        if (face == nullptr)
        {
            jassertfalse;
            return {};
        }

        HbFont hbFont { hb_font_create (face.get()) };
        FontStyleHelpers::initSynthetics (hbFont.get(), font);

        return new AndroidTypeface (DoCache::no, std::move (hbFont), font.getTypefaceName(), font.getTypefaceStyle());
    }

    enum class DoCache
    {
        no,
        yes
    };

    static Typeface::Ptr from (Span<const std::byte> blob, unsigned int index = 0)
    {
        return fromMemory (DoCache::yes, blob, index);
    }

    Native getNativeDetails() const override
    {
        return Native { hbFont.get() };
    }

    ~AndroidTypeface() override
    {
        if (doCache == DoCache::yes)
            if (auto* c = MemoryFontCache::getInstance())
                c->remove ({ getName(), getStyle() });
    }

    float getStringWidth (const String& text) override
    {
        const auto heightToPoints = getNativeDetails().getLegacyMetrics().getHeightToPointsFactor();
        const auto upem = hb_face_get_upem (hb_font_get_face (hbFont.get()));

        hb_position_t x{};
        doSimpleShape (text, [&] (const auto&, const auto& position)
        {
            x += position.x_advance;
        });
        return heightToPoints * (float) x / (float) upem;
    }

    void getGlyphPositions (const String& text, Array<int>& glyphs, Array<float>& xOffsets) override
    {
        const auto heightToPoints = getNativeDetails().getLegacyMetrics().getHeightToPointsFactor();
        const auto upem = hb_face_get_upem (hb_font_get_face (hbFont.get()));

        Point<hb_position_t> cursor{};

        doSimpleShape (text, [&] (const auto& info, const auto& position)
        {
            glyphs.add ((int) info.codepoint);
            xOffsets.add (heightToPoints * ((float) cursor.x + (float) position.x_offset) / (float) upem);
            cursor += Point { position.x_advance, position.y_advance };
        });

        xOffsets.add (heightToPoints * (float) cursor.x / (float) upem);
    }

private:
    template <typename Consumer>
    void doSimpleShape (const String& text, Consumer&& consumer)
    {
        HbBuffer buffer { hb_buffer_create() };
        hb_buffer_add_utf8 (buffer.get(), text.toRawUTF8(), -1, 0, -1);
        hb_buffer_guess_segment_properties (buffer.get());

        hb_shape (hbFont.get(), buffer.get(), nullptr, 0);

        unsigned int numGlyphs{};
        auto* infos = hb_buffer_get_glyph_infos (buffer.get(), &numGlyphs);
        auto* positions = hb_buffer_get_glyph_positions (buffer.get(), &numGlyphs);

        for (auto i = decltype (numGlyphs){}; i < numGlyphs; ++i)
            consumer (infos[i], positions[i]);
    }

    static Typeface::Ptr fromMemory (DoCache cache, Span<const std::byte> blob, unsigned int index = 0)
    {
        auto face = FontStyleHelpers::getFaceForBlob ({ reinterpret_cast<const char*> (blob.data()), blob.size() }, index);

        if (face == nullptr)
            return {};

        return new AndroidTypeface (cache,
                                    HbFont { hb_font_create (face.get()) },
                                    readFontName (face.get(), HB_OT_NAME_ID_FONT_FAMILY, nullptr),
                                    {});
    }

    static String readFontName (hb_face_t* face, hb_ot_name_id_t nameId, hb_language_t language)
    {
        unsigned int textSize{};
        textSize = hb_ot_name_get_utf8 (face, nameId, language, &textSize, nullptr);
        std::vector<char> nameString (textSize + 1, 0);
        textSize = (unsigned int) nameString.size();
        hb_ot_name_get_utf8 (face, nameId, language, &textSize, nameString.data());

        return nameString.data();
    }

    AndroidTypeface (DoCache cache, std::shared_ptr<hb_font_t> fontIn, const String& name, const String& style)
        : Typeface (name, style),
          hbFont (std::move (fontIn)),
          doCache (cache)
    {
        if (doCache == DoCache::yes)
            if (auto* c = MemoryFontCache::getInstance())
                c->add ({ name, style }, hbFont);
    }

    static MemoryBlock getBlobForFont (const Font& font)
    {
        auto memory = loadFontAsset (font.getTypefaceName());

        if (! memory.isEmpty())
            return memory;

        const auto file = findFontFile (font);

        if (! file.exists())
        {
            // Failed to find file corresponding to this font
            jassertfalse;
            return {};
        }

        FileInputStream stream { file };

        MemoryBlock result;
        stream.readIntoMemoryBlock (result);

        return stream.isExhausted() ? result : MemoryBlock{};
    }

    static File findFontFile (const Font& font)
    {
        const String styles[] { font.getTypefaceStyle(),
                                FontStyleHelpers::getStyleName (font.isBold(), font.isItalic()),
                                {} };

        for (const auto& style : styles)
            if (auto file = getFontFile (font.getTypefaceName(), style); file.exists())
                return file;

        for (auto& file : File ("/system/fonts").findChildFiles (File::findFiles, false, "*.ttf"))
            if (file.getFileName().startsWith (font.getTypefaceName()))
                return file;

        return {};
    }

    static File getFontFile (const String& family, const String& fontStyle)
    {
        return "/system/fonts/" + family + (fontStyle.isNotEmpty() ? ("-" + fontStyle) : String{}) + ".ttf";
    }

    static MemoryBlock loadFontAsset (const String& typefaceName)
    {
        auto* env = getEnv();

        const LocalRef<jobject> assetManager { env->CallObjectMethod (getAppContext().get(), AndroidContext.getAssets) };

        if (assetManager == nullptr)
            return {};

        const LocalRef<jobject> inputStream { env->CallObjectMethod (assetManager,
                                                                     AndroidAssetManager.open,
                                                                     javaString ("fonts/" + typefaceName).get()) };

        // Opening an input stream for an asset might throw if the asset isn't found
        jniCheckHasExceptionOccurredAndClear();

        if (inputStream == nullptr)
            return {};

        auto streamWrapper = makeAndroidInputStreamWrapper (inputStream);

        if (streamWrapper == nullptr)
            return {};

        MemoryBlock result;
        streamWrapper->readIntoMemoryBlock (result);

        return streamWrapper->isExhausted() ? result : MemoryBlock{};
    }

    std::shared_ptr<hb_font_t> hbFont;
    DoCache doCache;
};

//==============================================================================
Typeface::Ptr Typeface::createSystemTypefaceFor (const Font& font)
{
    return AndroidTypeface::from (font);
}

Typeface::Ptr Typeface::createSystemTypefaceFor (Span<const std::byte> data)
{
    return AndroidTypeface::from (data);
}

void Typeface::scanFolderForFonts (const File&)
{
    jassertfalse; // not currently available
}

bool TextLayout::createNativeLayout (const AttributedString&)
{
    return false;
}

} // namespace juce
