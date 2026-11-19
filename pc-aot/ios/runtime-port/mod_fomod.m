/* SPDX-License-Identifier: GPL-3.0-only
 * FOMOD (ModuleConfig.xml) installer: steps, groups and options with the
 * usual selection rules, flag/file conditions and conditional installs.
 * Files are copied from the app's extracted copy into the package's staging
 * Data folder; nothing outside those two folders is touched. */
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include "mod_install.h"
#include "mod_manager.h"
#include "mod_analyze.h"
#include "mod_archive.h"
#include "mod_deploy.h"

static _Atomic int japanese = -1;      /* also read by the background badge scan */
static void RefreshLanguage(void) {
    NSDictionary *s = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    japanese = [s[@"language"] isKindOfClass:NSNumber.class] && [s[@"language"] intValue] == 1;
}
static NSString *T(NSString *english, NSString *japaneseText) {
    if (japanese < 0) RefreshLanguage();
    return japanese ? japaneseText : english;
}

/* ---- minimal DOM ---------------------------------------------------------- */
@interface SKXNode : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSDictionary<NSString *, NSString *> *attrs;
@property(nonatomic, strong) NSMutableArray<SKXNode *> *children;
@property(nonatomic, strong) NSMutableString *text;
@end
@implementation SKXNode
- (instancetype)init { if ((self = [super init])) { _children = [NSMutableArray array]; _text = [NSMutableString string]; } return self; }
- (NSArray<SKXNode *> *)all:(NSString *)n {
    NSMutableArray *out = [NSMutableArray array];
    for (SKXNode *c in self.children) if ([c.name caseInsensitiveCompare:n] == NSOrderedSame) [out addObject:c];
    return out;
}
- (SKXNode *)child:(NSString *)n { return [self all:n].firstObject; }
- (NSString *)attr:(NSString *)n {
    for (NSString *k in self.attrs) if ([k caseInsensitiveCompare:n] == NSOrderedSame) return self.attrs[k];
    return nil;
}
- (NSString *)trimmed { return [self.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]; }
@end

@interface SKXBuilder : NSObject <NSXMLParserDelegate>
@property(nonatomic, strong) NSMutableArray<SKXNode *> *stack;
@property(nonatomic, strong) SKXNode *root;
@end
@implementation SKXBuilder
- (void)parser:(NSXMLParser *)p didStartElement:(NSString *)name namespaceURI:(NSString *)ns qualifiedName:(NSString *)q attributes:(NSDictionary *)attrs {
    (void)p; (void)ns; (void)q;
    SKXNode *n = [SKXNode new]; n.name = name; n.attrs = attrs;
    if (self.stack.count) [self.stack.lastObject.children addObject:n]; else self.root = n;
    [self.stack addObject:n];
}
- (void)parser:(NSXMLParser *)p didEndElement:(NSString *)name namespaceURI:(NSString *)ns qualifiedName:(NSString *)q {
    (void)p; (void)name; (void)ns; (void)q; [self.stack removeLastObject];
}
- (void)parser:(NSXMLParser *)p foundCharacters:(NSString *)s { (void)p; [self.stack.lastObject.text appendString:s]; }
@end
static SKXNode *ParseXML(NSData *data) {
    if (!data) return nil;
    NSXMLParser *parser = [[NSXMLParser alloc] initWithData:data];
    SKXBuilder *b = [SKXBuilder new]; b.stack = [NSMutableArray array];
    parser.delegate = b;
    parser.shouldResolveExternalEntities = NO;
    return [parser parse] ? b.root : nil;
}

/* ---- model ---------------------------------------------------------------- */
@interface SKFOption : NSObject
@property(nonatomic, copy) NSString *name, *desc, *image;
@property(nonatomic, copy) NSString *badges;          /* filled in the background */
@property(nonatomic, strong) UIImage *thumbnail;      /* filled in the background */
@property(nonatomic, strong) NSArray<NSDictionary *> *files;
@property(nonatomic, strong) NSDictionary<NSString *, NSString *> *flags;
@property(nonatomic, strong) SKXNode *typeNode;
@property(nonatomic) BOOL selected;
@end
@implementation SKFOption @end
@interface SKFGroup : NSObject
@property(nonatomic, copy) NSString *name, *type;
@property(nonatomic, strong) NSArray<SKFOption *> *options;
@end
@implementation SKFGroup @end
@interface SKFStep : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, strong) SKXNode *visible;
@property(nonatomic, strong) NSArray<SKFGroup *> *groups;
@end
@implementation SKFStep @end

static NSArray *Ordered(NSArray *items, NSString *order) {
    if ([order isEqualToString:@"Explicit"]) return items;
    NSArray *sorted = [items sortedArrayUsingComparator:^NSComparisonResult(id a, id b) { return [[a name] localizedStandardCompare:[b name]]; }];
    return [order isEqualToString:@"Descending"] ? sorted.reverseObjectEnumerator.allObjects : sorted;
}
static NSArray<NSDictionary *> *Files(SKXNode *files) {
    NSMutableArray *out = [NSMutableArray array];
    for (SKXNode *f in files.children) {
        BOOL folder = [f.name caseInsensitiveCompare:@"folder"] == NSOrderedSame;
        if (!folder && [f.name caseInsensitiveCompare:@"file"] != NSOrderedSame) continue;
        NSString *src = [f attr:@"source"] ?: @"";
        NSString *dst = [f attr:@"destination"];
        if (!dst) dst = folder ? @"" : src;
        [out addObject:@{@"source":src, @"destination":dst, @"folder":@(folder), @"priority":@([[f attr:@"priority"] integerValue])}];
    }
    return out;
}

@interface SKFomod : NSObject
@property(nonatomic, copy) NSString *title, *moduleImage;
@property(nonatomic, strong) NSArray<SKFStep *> *steps;
@property(nonatomic, strong) NSArray<NSDictionary *> *required;
@property(nonatomic, strong) NSArray<NSDictionary *> *conditional;   /* {deps, files} */
@property(nonatomic, strong) NSSet<NSString *> *active;
@property(nonatomic, copy) NSString *raw, *moduleRoot;
@end
@implementation SKFomod
- (instancetype)initWithConfig:(NSString *)path active:(NSSet *)active raw:(NSString *)raw {
    SKXNode *root = ParseXML([NSData dataWithContentsOfFile:path]);
    if (!root || [root.name caseInsensitiveCompare:@"config"] != NSOrderedSame) return nil;
    if (!(self = [super init])) return nil;
    _active = active; _raw = raw;
    _moduleRoot = [path.stringByDeletingLastPathComponent stringByDeletingLastPathComponent];
    _title = [[root child:@"moduleName"] trimmed] ?: @"MOD";
    _moduleImage = [[root child:@"moduleImage"] attr:@"path"];
    _required = Files([root child:@"requiredInstallFiles"]);
    NSMutableArray *conditional = [NSMutableArray array];
    for (SKXNode *pattern in [[[root child:@"conditionalFileInstalls"] child:@"patterns"] all:@"pattern"])
        [conditional addObject:@{@"deps":[pattern child:@"dependencies"] ?: [SKXNode new], @"files":Files([pattern child:@"files"])}];
    _conditional = conditional;
    NSMutableArray *steps = [NSMutableArray array];
    SKXNode *installSteps = [root child:@"installSteps"];
    for (SKXNode *s in [installSteps all:@"installStep"]) {
        SKFStep *step = [SKFStep new];
        step.name = [s attr:@"name"] ?: @"";
        step.visible = [[s child:@"visible"] child:@"dependencies"] ?: [s child:@"visible"];
        NSMutableArray *groups = [NSMutableArray array];
        SKXNode *gs = [s child:@"optionalFileGroups"];
        for (SKXNode *g in [gs all:@"group"]) {
            SKFGroup *group = [SKFGroup new];
            group.name = [g attr:@"name"] ?: @""; group.type = [g attr:@"type"] ?: @"SelectAny";
            NSMutableArray *options = [NSMutableArray array];
            SKXNode *plugins = [g child:@"plugins"];
            for (SKXNode *o in [plugins all:@"plugin"]) {
                SKFOption *opt = [SKFOption new];
                opt.name = [o attr:@"name"] ?: @"";
                opt.desc = [[o child:@"description"] trimmed] ?: @"";
                opt.image = [[o child:@"image"] attr:@"path"];
                opt.files = Files([o child:@"files"]);
                NSMutableDictionary *flags = [NSMutableDictionary dictionary];
                for (SKXNode *f in [[o child:@"conditionFlags"] all:@"flag"]) if ([f attr:@"name"]) flags[[f attr:@"name"]] = [f trimmed];
                opt.flags = flags;
                opt.typeNode = [o child:@"typeDescriptor"];
                [options addObject:opt];
            }
            group.options = Ordered(options, [plugins attr:@"order"]);
            [groups addObject:group];
        }
        step.groups = Ordered(groups, [gs attr:@"order"]);
        [steps addObject:step];
    }
    _steps = Ordered(steps, [installSteps attr:@"order"]);
    for (SKFStep *step in _steps) for (SKFGroup *g in step.groups) [self applyDefaults:g flags:@{}];
    return self;
}
/* Flags set by selected options of visible steps before `limit`. */
- (NSDictionary *)flagsBefore:(NSUInteger)limit {
    NSMutableDictionary *flags = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < limit && i < self.steps.count; ++i) {
        if (![self deps:self.steps[i].visible flags:flags]) continue;
        for (SKFGroup *g in self.steps[i].groups) for (SKFOption *o in g.options) if (o.selected) [flags addEntriesFromDictionary:o.flags];
    }
    return flags;
}
- (BOOL)fileState:(NSString *)file is:(NSString *)wanted {
    NSString *name = file.lastPathComponent.lowercaseString;
    BOOL present = [self.active containsObject:name];
    if ([wanted caseInsensitiveCompare:@"Missing"] == NSOrderedSame) return !present;
    if ([wanted caseInsensitiveCompare:@"Inactive"] == NSOrderedSame) return NO;   /* unknown: be conservative */
    return present;
}
- (BOOL)deps:(SKXNode *)deps flags:(NSDictionary *)flags {
    if (!deps || !deps.children.count) return YES;
    BOOL any = [[deps attr:@"operator"] caseInsensitiveCompare:@"Or"] == NSOrderedSame;
    for (SKXNode *d in deps.children) {
        BOOL ok = YES;
        NSString *n = d.name.lowercaseString;
        if ([n isEqualToString:@"flagdependency"]) ok = [(flags[[d attr:@"flag"] ?: @""] ?: @"") isEqualToString:[d attr:@"value"] ?: @""];
        else if ([n isEqualToString:@"filedependency"]) ok = [self fileState:[d attr:@"file"] ?: @"" is:[d attr:@"state"] ?: @"Active"];
        else if ([n isEqualToString:@"dependencies"]) ok = [self deps:d flags:flags];
        /* gameDependency / fommDependency and unknown checks pass. */
        if (any && ok) return YES;
        if (!any && !ok) return NO;
    }
    return !any;
}
- (NSString *)typeOf:(SKFOption *)o flags:(NSDictionary *)flags {
    SKXNode *t = o.typeNode;
    if ([t child:@"type"]) return [[t child:@"type"] attr:@"name"] ?: @"Optional";
    SKXNode *dt = [t child:@"dependencyType"];
    for (SKXNode *p in [[dt child:@"patterns"] all:@"pattern"])
        if ([self deps:[p child:@"dependencies"] flags:flags]) return [[p child:@"type"] attr:@"name"] ?: @"Optional";
    return [[dt child:@"defaultType"] attr:@"name"] ?: @"Optional";
}
- (void)applyDefaults:(SKFGroup *)g flags:(NSDictionary *)flags {
    BOOL single = [g.type isEqualToString:@"SelectExactlyOne"] || [g.type isEqualToString:@"SelectAtMostOne"];
    SKFOption *first = nil;
    for (SKFOption *o in g.options) {
        NSString *type = [self typeOf:o flags:flags];
        o.selected = [g.type isEqualToString:@"SelectAll"] || [type isEqualToString:@"Required"] ||
                     ([type isEqualToString:@"Recommended"] && !(single && first));
        if ([type isEqualToString:@"NotUsable"]) o.selected = NO;
        if (o.selected && !first) first = o;
        else if (single && o.selected) o.selected = NO;
    }
    if (!first && ([g.type isEqualToString:@"SelectExactlyOne"] || [g.type isEqualToString:@"SelectAtLeastOne"]))
        for (SKFOption *o in g.options) if (![[self typeOf:o flags:flags] isEqualToString:@"NotUsable"]) { o.selected = YES; break; }
}
- (NSArray<NSNumber *> *)visibleStepIndexes {
    NSMutableArray *out = [NSMutableArray array];
    for (NSUInteger i = 0; i < self.steps.count; ++i)
        if ([self deps:self.steps[i].visible flags:[self flagsBefore:i]]) [out addObject:@(i)];
    return out;
}
- (NSArray<NSDictionary *> *)plan {
    NSMutableArray *ops = [NSMutableArray arrayWithArray:self.required];
    for (NSNumber *i in self.visibleStepIndexes)
        for (SKFGroup *g in self.steps[i.unsignedIntegerValue].groups)
            for (SKFOption *o in g.options) if (o.selected) [ops addObjectsFromArray:o.files];
    NSDictionary *flags = [self flagsBefore:self.steps.count];
    for (NSDictionary *c in self.conditional) if ([self deps:c[@"deps"] flags:flags]) [ops addObjectsFromArray:c[@"files"]];
    /* Stable by priority: later and higher priority win. */
    NSMutableArray *indexed = [NSMutableArray array];
    for (NSUInteger i = 0; i < ops.count; ++i) [indexed addObject:@[ops[i], @(i)]];
    [indexed sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) {
        NSInteger pa = [a[0][@"priority"] integerValue], pb = [b[0][@"priority"] integerValue];
        if (pa != pb) return pa < pb ? NSOrderedAscending : NSOrderedDescending;
        return [a[1] compare:b[1]];
    }];
    NSMutableArray *out = [NSMutableArray array];
    for (NSArray *x in indexed) [out addObject:x[0]];
    return out;
}
@end

/* Resolve an author's path inside the extracted archive, ignoring case. */
static NSString *ResolveCI(NSString *root, NSString *relative) {
    NSString *current = root;
    for (NSString *raw in [[relative stringByReplacingOccurrencesOfString:@"\\" withString:@"/"] componentsSeparatedByString:@"/"]) {
        if (!raw.length || [raw isEqualToString:@"."]) continue;
        if ([raw isEqualToString:@".."]) return nil;
        NSString *match = nil;
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:current error:NULL])
            if ([name caseInsensitiveCompare:raw] == NSOrderedSame) { match = name; break; }
        if (!match) return nil;
        current = [current stringByAppendingPathComponent:match];
    }
    return current;
}
static BOOL PlaceFile(NSString *src, NSString *data, NSString *dest) {
    char safe[1024];
    const char *d = dest.UTF8String;
    if (skyrim_zip_normalize(d, strlen(d), 1, safe, sizeof safe) != 1) return NO;
    NSString *target = [data stringByAppendingPathComponent:@(safe)];
    [NSFileManager.defaultManager removeItemAtPath:target error:NULL];        /* later operations win */
    return !skyrim_make_parents(data.fileSystemRepresentation, safe) &&
           !skyrim_clone_file(src.fileSystemRepresentation, target.fileSystemRepresentation);
}
static BOOL Execute(SKFomod *f, NSArray<NSDictionary *> *plan, NSString *data) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSDictionary *op in plan) {
        NSString *src = ResolveCI(f.moduleRoot, op[@"source"]);
        if (!src) { fprintf(stderr, "IOS_MODS fomod missing source\n"); continue; }      /* authors' typos: skip */
        NSString *dest = [op[@"destination"] stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
        if ([op[@"folder"] boolValue]) {
            NSDirectoryEnumerator *en = [fm enumeratorAtPath:src];
            for (NSString *rel in en) {
                if (![en.fileAttributes[NSFileType] isEqual:NSFileTypeRegular]) continue;
                if (!PlaceFile([src stringByAppendingPathComponent:rel], data, dest.length ? [dest stringByAppendingPathComponent:rel] : rel)) return NO;
            }
        } else {
            if (!dest.length || [dest hasSuffix:@"/"]) dest = [dest stringByAppendingString:src.lastPathComponent];
            if (!PlaceFile(src, data, dest)) return NO;
        }
    }
    return YES;
}

/* ---- help for reading the choices ------------------------------------------ */
/* Common installer words, used when on-device translation is unavailable or
 * has not finished. Whole-word replacements only; anything else stays as the
 * author wrote it. */
static NSDictionary<NSString *, NSString *> *Glossary(void) {
    static NSDictionary *g;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g = @{@"main file":@"本体", @"main files":@"本体", @"main":@"本体", @"core":@"本体", @"core files":@"本体",
              @"optional":@"任意", @"optional files":@"任意ファイル", @"options":@"オプション", @"option":@"オプション",
              @"required":@"必須", @"recommended":@"推奨", @"patch":@"パッチ", @"patches":@"パッチ",
              @"compatibility":@"互換性", @"compatibility patches":@"互換パッチ", @"textures":@"テクスチャ",
              @"texture":@"テクスチャ", @"meshes":@"メッシュ", @"mesh":@"メッシュ", @"performance":@"軽量（動作重視）",
              @"quality":@"高画質", @"high quality":@"高画質", @"low":@"低", @"medium":@"中", @"high":@"高",
              @"ultra":@"最高", @"vanilla":@"バニラ（元のまま）", @"default":@"標準", @"none":@"なし",
              @"resolution":@"解像度", @"texture resolution":@"テクスチャ解像度", @"512":@"512（とても軽い）",
              @"1k":@"1K（軽い）", @"2k":@"2K（標準）", @"4k":@"4K（重い）", @"8k":@"8K（非常に重い）",
              @"lore friendly":@"世界観重視", @"immersive":@"没入感重視", @"add-ons":@"追加要素", @"addons":@"追加要素",
              @"plugins":@"プラグイン", @"plugin":@"プラグイン", @"settings":@"設定", @"version":@"バージョン",
              @"male":@"男性", @"female":@"女性", @"armor":@"防具", @"armour":@"防具", @"weapons":@"武器",
              @"dawnguard":@"ドーンガード", @"dragonborn":@"ドラゴンボーン", @"hearthfire":@"ハースファイア",
              @"hearthfires":@"ハースファイア", @"unofficial patch":@"非公式パッチ", @"special edition":@"SE版",
              @"legendary edition":@"レジェンダリー版", @"welcome":@"ようこそ", @"introduction":@"はじめに",
              @"intro":@"はじめに", @"finish":@"完了", @"readme":@"説明書", @"color":@"色", @"colour":@"色",
              @"standard":@"標準", @"lite":@"軽量版", @"light":@"軽量", @"full":@"フル", @"all":@"すべて"};
    });
    return g;
}
static NSString *GlossaryText(NSString *text) {
    NSString *trimmed = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *exact = Glossary()[trimmed.lowercaseString];
    if (exact) return exact;
    if (trimmed.length > 60) return nil;                       /* sentences: leave to translation */
    static NSRegularExpression *terms;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSArray *keys = [Glossary().allKeys sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
            return [@(b.length) compare:@(a.length)];
        }];
        NSMutableArray *escaped = [NSMutableArray arrayWithCapacity:keys.count];
        for (NSString *key in keys) [escaped addObject:[NSRegularExpression escapedPatternForString:key]];
        NSString *pattern = [NSString stringWithFormat:@"(?<![A-Za-z0-9])(?:%@)(?![A-Za-z0-9])", [escaped componentsJoinedByString:@"|"]];
        terms = [NSRegularExpression regularExpressionWithPattern:pattern options:NSRegularExpressionCaseInsensitive error:NULL];
    });
    NSArray *matches = [terms matchesInString:trimmed options:0 range:NSMakeRange(0, trimmed.length)];
    if (!matches.count) return nil;
    NSMutableString *out = [trimmed mutableCopy];
    for (NSTextCheckingResult *match in matches.reverseObjectEnumerator) {
        NSString *replacement = Glossary()[[trimmed substringWithRange:match.range].lowercaseString];
        if (replacement) [out replaceCharactersInRange:match.range withString:replacement];
    }
    return out;
}
static NSString *TypeLabel(NSString *type) {
    NSDictionary *labels = @{@"Required":T(@"required", @"必須"), @"Recommended":T(@"recommended", @"推奨"),
        @"NotUsable":T(@"not usable", @"使用不可"), @"CouldBeUsable":T(@"may work", @"条件付き")};
    return labels[type];
}

/* What would this option install, read from the actual files (background). */
static NSString *OptionBadges(SKFomod *f, SKFOption *o, BOOL optimize) {
    SkyrimModReport r;
    memset(&r, 0, sizeof r);
    NSUInteger ops = 0;
    for (NSDictionary *op in o.files) {
        NSString *src = ResolveCI(f.moduleRoot, op[@"source"]);
        if (!src) continue;
        ++ops;
        NSString *dest = [op[@"destination"] stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
        if ([op[@"folder"] boolValue]) skyrim_mod_analyze_add_dir(src.fileSystemRepresentation, dest.length == 0, &r);
        else {
            if (!dest.length || [dest hasSuffix:@"/"]) dest = [dest stringByAppendingString:src.lastPathComponent];
            skyrim_mod_analyze_add_file(src.fileSystemRepresentation, dest.UTF8String, &r);
        }
    }
    NSMutableArray *bad = [NSMutableArray array], *info = [NSMutableArray array];
    if (r.skse_dlls) [bad addObject:T(@"⛔ SKSE DLL: won't run on iPhone", @"⛔ SKSEのDLL：iPhoneでは動きません")];
    if (r.se_plugins || r.esl) [bad addObject:T(@"⚠️ Special Edition plugin", @"⚠️ SE用プラグイン")];
    if (r.se_meshes) [bad addObject:T(@"⚠️ Special Edition meshes", @"⚠️ SE用メッシュ")];
    if (r.skse_scripts) [bad addObject:T(@"⚠️ needs SKSE", @"⚠️ SKSEが必要")];
    if (r.mcm_scripts) [info addObject:T(@"SkyUI menu (MCM)", @"SkyUIのMCM用")];
    if (r.textures_4k && optimize) [info addObject:T(@"4K textures (reduced automatically)", @"4Kテクスチャ（自動で縮小）")];
    else if (r.textures_4k) [bad addObject:T(@"⚠️ 4K textures: heavy", @"⚠️ 4Kテクスチャ：重い")];
    else if (r.texture_bytes >= (512ull << 20)) [bad addObject:T(@"⚠️ heavy textures", @"⚠️ テクスチャが重い")];
    if (optimize && (r.bc7_textures || r.se_bsas)) [info addObject:T(@"SE formats converted automatically", @"SE形式は自動で変換")];
    NSString *text = [NSString stringWithFormat:@"%@ %@", o.name ?: @"", o.desc ?: @""];
    if (!r.skse_dlls && !r.skse_scripts && [text rangeOfString:@"SKSE" options:NSCaseInsensitiveSearch].location != NSNotFound)
        [info addObject:T(@"mentions SKSE", @"説明にSKSEの記載あり")];
    NSRegularExpression *se = [NSRegularExpression regularExpressionWithPattern:@"\\b(SSE|Special Edition|Anniversary Edition)\\b" options:NSRegularExpressionCaseInsensitive error:NULL];
    if (!r.se_plugins && [se firstMatchInString:text options:0 range:NSMakeRange(0, text.length)])
        [info addObject:T(@"mentions Special Edition", @"説明にSE向けの記載あり")];
    if (!ops) [info addObject:T(@"no files (setting only)", @"ファイルなし（設定のみ）")];
    else if (!bad.count) [bad addObject:T(@"✅ looks fine on iPhone", @"✅ iPhoneで使えそう")];
    [bad addObjectsFromArray:info];
    return [bad componentsJoinedByString:@"　"];
}
static UIImage *Thumbnail(NSString *path, CGFloat side) {
    UIImage *image = path ? [UIImage imageWithContentsOfFile:path] : nil;
    if (!image || image.size.width < 1 || image.size.height < 1) return nil;
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        (void)ctx;
        CGFloat scale = MAX(side / image.size.width, side / image.size.height);
        CGSize size = CGSizeMake(image.size.width * scale, image.size.height * scale);
        [image drawInRect:CGRectMake((side - size.width) / 2, (side - size.height) / 2, size.width, size.height)];
    }];
}

/* Full text, selectable, so iOS's own "Translate" menu also works. */
@interface SKTextSheet : UIViewController
@property(nonatomic, copy) NSString *text;
@end
@implementation SKTextSheet
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UITextView *tv = [[UITextView alloc] initWithFrame:self.view.bounds];
    tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    tv.editable = NO; tv.selectable = YES; tv.text = self.text;
    tv.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    tv.textContainerInset = UIEdgeInsetsMake(20, 16, 20, 16);
    [self.view addSubview:tv];
}
@end

/* ---- screens ---------------------------------------------------------------- */
@interface SKFomodController : UITableViewController
@property(nonatomic, strong) SKFomod *fomod;
@property(nonatomic, strong) NSArray<NSNumber *> *visible;
@property(nonatomic) NSUInteger position;
@property(nonatomic, copy) NSString *data, *modName;
@property(nonatomic, copy) void (^completion)(BOOL, NSString *);
@property(nonatomic) BOOL finished, showOriginal, translationRequested, translating, optimize;
@property(nonatomic, strong) NSMutableIndexSet *visited;   /* steps whose defaults were applied */
@property(nonatomic, strong) NSDictionary<NSString *, NSString *> *translations;
@property(nonatomic, strong) UIImage *moduleImage;
@end
@implementation SKFomodController
- (SKFStep *)step { return self.fomod.steps[self.visible[self.position].unsignedIntegerValue]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.optimize = ![NSUserDefaults.standardUserDefaults boolForKey:@"mods-optimize-off"];
    self.tableView.estimatedRowHeight = 90; self.tableView.rowHeight = UITableViewAutomaticDimension;
    [self.tableView addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(longPress:)]];
    NSString *imagePath = self.fomod.moduleImage.length ? ResolveCI(self.fomod.moduleRoot, self.fomod.moduleImage) : nil;
    if (imagePath)
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            UIImage *image = [UIImage imageWithContentsOfFile:imagePath];
            dispatch_async(dispatch_get_main_queue(), ^{ self.moduleImage = image; [self updateHeader]; });
        });
    [self refresh];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self requestTranslation];      /* needs a window for the language download prompt */
}
/* Display text: on-device translation, else glossary, else original. */
- (NSString *)show:(NSString *)text {
    if (!text.length || !japanese || self.showOriginal) return text;
    return self.translations[text] ?: GlossaryText(text) ?: text;
}
- (void)requestTranslation {
    if (self.translationRequested || !japanese) return;
    self.translationRequested = YES;
    Class translator = NSClassFromString(@"SKTranslator");
    SEL sel = NSSelectorFromString(@"translate:in:completion:");
    if (![translator respondsToSelector:sel]) return;
    NSMutableOrderedSet *texts = [NSMutableOrderedSet orderedSet];
    NSCharacterSet *latin = [NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"];
    void (^add)(NSString *) = ^(NSString *t) { if (t.length && [t rangeOfCharacterFromSet:latin].location != NSNotFound) [texts addObject:t]; };
    add(self.fomod.title);
    for (SKFStep *step in self.fomod.steps) {
        add(step.name);
        for (SKFGroup *g in step.groups) { add(g.name); for (SKFOption *o in g.options) { add(o.name); add(o.desc); } }
    }
    if (!texts.count || texts.count > 600) return;
    self.translating = YES; [self updateHeader];
    NSArray *list = texts.array;
    __weak SKFomodController *weakController = self;
    void (^done)(NSArray *) = ^(NSArray *result) {
        SKFomodController *controller = weakController;
        if (!controller || controller.finished) return;
        controller.translating = NO;
        if (result.count == list.count) {
            NSMutableDictionary *map = [NSMutableDictionary dictionary];
            for (NSUInteger i = 0; i < list.count; ++i) if ([result[i] isKindOfClass:NSString.class]) map[list[i]] = result[i];
            controller.translations = map;
        }
        [controller refresh];
    };
    ((void (*)(id, SEL, NSArray *, UIViewController *, void (^)(NSArray *)))objc_msgSend)(translator, sel, list, self, done);
}
- (void)toggleOriginal { self.showOriginal = !self.showOriginal; [self refresh]; }
- (void)updateHeader {
    CGFloat width = self.tableView.bounds.size.width > 0 ? self.tableView.bounds.size.width : UIScreen.mainScreen.bounds.size.width;
    UIView *header = [[UIView alloc] initWithFrame:CGRectMake(0, 0, width, 10)];
    CGFloat y = 12;
    if (self.moduleImage) {
        UIImageView *iv = [[UIImageView alloc] initWithFrame:CGRectMake(16, y, width - 32, 150)];
        iv.image = self.moduleImage; iv.contentMode = UIViewContentModeScaleAspectFit;
        iv.layer.cornerRadius = 10; iv.clipsToBounds = YES;
        [header addSubview:iv]; y += 158;
    }
    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(20, y, width - 40, 10)];
    label.numberOfLines = 0;
    label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    label.textColor = UIColor.secondaryLabelColor;
    NSMutableArray *lines = [NSMutableArray array];
    NSString *module = [self show:self.fomod.title];
    if (module.length && ![module isEqualToString:self.modName]) [lines addObject:[NSString stringWithFormat:T(@"Installer: %@", @"インストーラー：%@"), module]];
    if (self.visible.count) [lines addObject:[NSString stringWithFormat:T(@"Step %lu of %lu: %@", @"ステップ %lu／%lu：%@"),
        (unsigned long)self.position + 1, (unsigned long)self.visible.count, [self show:self.step.name] ?: @""]];
    if (self.translating) [lines addObject:T(@"Translating…", @"日本語に翻訳中…")];
    else if (japanese && !self.showOriginal) [lines addObject:self.translations ? T(@"Translated on this iPhone. Long-press an option for the full text.", @"iPhone内で翻訳しています。長押しで原文と全文を表示。")
                                                                           : T(@"Long-press an option for the full text.", @"項目を長押しすると全文を表示します。")];
    label.text = [lines componentsJoinedByString:@"\n"];
    CGSize fit = [label sizeThatFits:CGSizeMake(width - 40, CGFLOAT_MAX)];
    label.frame = CGRectMake(20, y, width - 40, fit.height);
    [header addSubview:label];
    header.frame = CGRectMake(0, 0, width, y + fit.height + 8);
    self.tableView.tableHeaderView = header;
}
- (void)refresh {
    self.visible = self.fomod.visibleStepIndexes;
    if (self.position >= self.visible.count) self.position = self.visible.count ? self.visible.count - 1 : 0;
    BOOL last = self.position + 1 >= self.visible.count;
    /* The MOD's own name stays above the title on every step. */
    self.navigationItem.prompt = self.modName;
    self.title = self.visible.count ? [NSString stringWithFormat:T(@"Step %lu/%lu", @"ステップ %lu/%lu"), (unsigned long)self.position + 1, (unsigned long)self.visible.count]
                                    : T(@"Install", @"インストール");
    UIBarButtonItem *back = [[UIBarButtonItem alloc] initWithTitle:self.position ? T(@"Back", @"戻る") : T(@"Cancel", @"キャンセル")
        style:UIBarButtonItemStylePlain target:self action:@selector(back)];
    NSMutableArray *left = [NSMutableArray arrayWithObject:back];
    if (japanese) [left addObject:[[UIBarButtonItem alloc] initWithTitle:self.showOriginal ? @"日本語" : @"原文"
        style:UIBarButtonItemStylePlain target:self action:@selector(toggleOriginal)]];
    self.navigationItem.leftBarButtonItems = left;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:last ? T(@"Install", @"インストール") : T(@"Next", @"次へ")
        style:UIBarButtonItemStyleDone target:self action:@selector(next)];
    [self updateHeader];
    [self.tableView reloadData];
    [self loadStepDetails];
}
/* Badges and thumbnails for the shown step, computed off the main thread. */
- (void)loadStepDetails {
    if (!self.visible.count) return;
    NSMutableArray<SKFOption *> *pending = [NSMutableArray array];
    for (SKFGroup *g in self.step.groups) for (SKFOption *o in g.options) if (!o.badges) [pending addObject:o];
    if (!pending.count) return;
    for (SKFOption *o in pending) o.badges = T(@"Checking files…", @"ファイルを確認中…");
    SKFomod *f = self.fomod; BOOL optimize = self.optimize;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (SKFOption *o in pending) {
            NSString *badges = OptionBadges(f, o, optimize);
            NSString *imagePath = o.image.length ? ResolveCI(f.moduleRoot, o.image) : nil;
            UIImage *thumb = imagePath ? Thumbnail(imagePath, 64) : nil;
            dispatch_async(dispatch_get_main_queue(), ^{ o.badges = badges; o.thumbnail = thumb; [self.tableView reloadData]; });
        }
    });
}
- (void)finish:(BOOL)ok message:(NSString *)m {
    if (self.finished) return;
    self.finished = YES;
    void (^done)(BOOL, NSString *) = self.completion;
    [self dismissViewControllerAnimated:YES completion:^{
        skyrim_mobile_menu_pause(SKYRIM_MENU_PAUSE_INSTALLER, 0);
        done(ok, m);
    }];
}
- (void)back {
    if (!self.position) { [self finish:NO message:T(@"Installation cancelled.", @"インストールを中止しました。")]; return; }
    --self.position; [self refresh];
}
- (BOOL)stepValid {
    if (!self.visible.count) return YES;
    for (SKFGroup *g in self.step.groups) {
        NSUInteger on = 0; for (SKFOption *o in g.options) on += o.selected;
        if (([g.type isEqualToString:@"SelectExactlyOne"] && on != 1) || ([g.type isEqualToString:@"SelectAtLeastOne"] && !on)) return NO;
    }
    return YES;
}
- (void)next {
    if (![self stepValid]) {
        UIAlertController *a = [UIAlertController alertControllerWithTitle:T(@"Choose an option in each required group", @"必要な項目を選んでください") message:nil preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:a animated:YES completion:nil];
        return;
    }
    if (self.position + 1 < self.visible.count) {
        ++self.position;
        NSUInteger index = self.visible[self.position].unsignedIntegerValue;
        if (!self.visited) self.visited = [NSMutableIndexSet indexSet];
        if (![self.visited containsIndex:index]) {
            /* Defaults depend on earlier choices; apply once, keep later edits. */
            NSDictionary *flags = [self.fomod flagsBefore:index];
            for (SKFGroup *g in self.step.groups) [self.fomod applyDefaults:g flags:flags];
            [self.visited addIndex:index];
        }
        [self refresh];
        return;
    }
    NSArray *plan = self.fomod.plan;
    self.navigationItem.rightBarButtonItem.enabled = NO;
    SKFomod *f = self.fomod; NSString *data = self.data;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL ok = Execute(f, plan, data);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self finish:ok message:ok ? nil : T(@"Could not copy the selected files.", @"選んだファイルをコピーできませんでした。")];
        });
    });
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)t { (void)t; return self.visible.count ? (NSInteger)self.step.groups.count : 1; }
- (NSInteger)tableView:(UITableView *)t numberOfRowsInSection:(NSInteger)s {
    (void)t; return self.visible.count ? (NSInteger)self.step.groups[s].options.count : 1;
}
- (NSString *)tableView:(UITableView *)t titleForHeaderInSection:(NSInteger)s {
    (void)t;
    if (!self.visible.count) return nil;
    SKFGroup *g = self.step.groups[s];
    NSDictionary *rule = @{@"SelectExactlyOne":T(@"choose one", @"1つ選択"), @"SelectAtMostOne":T(@"choose up to one", @"0〜1つ選択"),
        @"SelectAtLeastOne":T(@"choose at least one", @"1つ以上選択"), @"SelectAll":T(@"all included", @"すべて含まれます"), @"SelectAny":T(@"choose any", @"自由に選択")};
    return [NSString stringWithFormat:@"%@ — %@", [self show:g.name], rule[g.type] ?: g.type];
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    UITableViewCell *c = [t dequeueReusableCellWithIdentifier:@"SKFomodCell"];
    if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"SKFomodCell"];
    c.textLabel.numberOfLines = 2; c.detailTextLabel.numberOfLines = 7; c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    c.imageView.image = nil; c.textLabel.enabled = YES;
    if (!self.visible.count) {
        c.textLabel.text = T(@"No choices needed. Tap Install.", @"選択する項目はありません。「インストール」を押してください。");
        c.detailTextLabel.text = nil; c.accessoryType = UITableViewCellAccessoryNone;
        return c;
    }
    SKFOption *o = self.step.groups[p.section].options[p.row];
    NSString *type = [self.fomod typeOf:o flags:[self.fomod flagsBefore:self.visible[self.position].unsignedIntegerValue]];
    NSString *name = [self show:o.name], *typeText = TypeLabel(type);
    c.textLabel.text = typeText && ![type isEqualToString:@"Optional"] ? [NSString stringWithFormat:@"%@（%@）", name, typeText] : name;
    NSMutableArray *lines = [NSMutableArray array];
    if (![name isEqualToString:o.name]) [lines addObject:o.name];             /* original name for searching */
    if (o.badges.length) [lines addObject:o.badges];
    NSString *desc = [self show:o.desc];
    if (desc.length) [lines addObject:desc];
    c.detailTextLabel.text = [lines componentsJoinedByString:@"\n"];
    c.imageView.image = o.thumbnail;
    c.accessoryType = o.selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    BOOL locked = [type isEqualToString:@"Required"] || [type isEqualToString:@"NotUsable"] || [self.step.groups[p.section].type isEqualToString:@"SelectAll"];
    c.textLabel.enabled = ![type isEqualToString:@"NotUsable"];
    c.selectionStyle = locked ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
    return c;
}
- (void)longPress:(UILongPressGestureRecognizer *)g {
    if (g.state != UIGestureRecognizerStateBegan || !self.visible.count) return;
    NSIndexPath *p = [self.tableView indexPathForRowAtPoint:[g locationInView:self.tableView]];
    if (!p) return;
    SKFOption *o = self.step.groups[p.section].options[p.row];
    NSMutableArray *parts = [NSMutableArray array];
    NSString *name = [self show:o.name];
    [parts addObject:name];
    if (![name isEqualToString:o.name]) [parts addObject:o.name];
    if (o.badges.length) [parts addObject:o.badges];
    if (o.desc.length) {
        NSString *description = [self show:o.desc];
        if (![description isEqualToString:o.desc]) {
            [parts addObject:description];
            [parts addObject:[@"――原文――\n" stringByAppendingString:o.desc]];
        } else {
            [parts addObject:o.desc];
        }
    }
    SKTextSheet *sheet = [SKTextSheet new];
    sheet.text = [parts componentsJoinedByString:@"\n\n"];
    sheet.sheetPresentationController.detents = @[UISheetPresentationControllerDetent.mediumDetent, UISheetPresentationControllerDetent.largeDetent];
    sheet.sheetPresentationController.prefersGrabberVisible = YES;
    [self presentViewController:sheet animated:YES completion:nil];
}
- (void)tableView:(UITableView *)t didSelectRowAtIndexPath:(NSIndexPath *)p {
    [t deselectRowAtIndexPath:p animated:YES];
    if (!self.visible.count) return;
    SKFGroup *g = self.step.groups[p.section];
    SKFOption *o = g.options[p.row];
    NSString *type = [self.fomod typeOf:o flags:[self.fomod flagsBefore:self.visible[self.position].unsignedIntegerValue]];
    if ([type isEqualToString:@"Required"] || [type isEqualToString:@"NotUsable"] || [g.type isEqualToString:@"SelectAll"]) return;
    if ([g.type isEqualToString:@"SelectExactlyOne"]) { for (SKFOption *x in g.options) x.selected = x == o; }
    else if ([g.type isEqualToString:@"SelectAtMostOne"]) { BOOL was = o.selected; for (SKFOption *x in g.options) x.selected = NO; o.selected = !was; }
    else o.selected = !o.selected;
    [t reloadSections:[NSIndexSet indexSetWithIndex:p.section] withRowAnimation:UITableViewRowAnimationNone];
}
@end

static UIViewController *TopController(void) {
    UIViewController *top = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *w in ((UIWindowScene *)scene).windows) if (w.isKeyWindow) top = w.rootViewController;
    }
    while (top.presentedViewController) top = top.presentedViewController;
    return top;
}

void skyrim_fomod_run(NSString *mod_name, NSString *raw, NSString *config, NSString *data, NSSet<NSString *> *active,
                      void (^completion)(BOOL, NSString *)) {
    RefreshLanguage();
    SKFomod *f = [[SKFomod alloc] initWithConfig:config active:active raw:raw];
    if (!f) { completion(NO, T(@"The installer file could not be read.", @"インストーラーの設定ファイルを読めませんでした。")); return; }
    UIViewController *top = TopController();
    if (!top) { completion(NO, T(@"Open the app to run the installer.", @"インストーラーを表示できませんでした。")); return; }
    SKFomodController *c = [[SKFomodController alloc] initWithStyle:UITableViewStyleInsetGrouped];
    c.fomod = f; c.data = data; c.completion = completion; c.modName = mod_name;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:c];
    nav.modalInPresentation = YES;
    /* Pause the game while choosing, like the MOD screens. */
    skyrim_mobile_menu_pause(SKYRIM_MENU_PAUSE_INSTALLER, 1);
    [top presentViewController:nav animated:YES completion:nil];
}
