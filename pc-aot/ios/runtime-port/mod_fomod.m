/* SPDX-License-Identifier: GPL-3.0-only
 * FOMOD (ModuleConfig.xml) installer: steps, groups and options with the
 * usual selection rules, flag/file conditions and conditional installs.
 * Files are copied from the app's extracted copy into the package's staging
 * Data folder; nothing outside those two folders is touched. */
#import <UIKit/UIKit.h>
#include "mod_install.h"
#include "mod_archive.h"
#include "mod_deploy.h"

static NSString *T(NSString *english, NSString *japanese) {
    NSDictionary *s = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    return [s[@"language"] isKindOfClass:NSNumber.class] && [s[@"language"] intValue] == 1 ? japanese : english;
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
@property(nonatomic, copy) NSString *name, *desc;
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
@property(nonatomic, copy) NSString *title;
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

/* ---- screens ---------------------------------------------------------------- */
@interface SKFomodController : UITableViewController
@property(nonatomic, strong) SKFomod *fomod;
@property(nonatomic, strong) NSArray<NSNumber *> *visible;
@property(nonatomic) NSUInteger position;
@property(nonatomic, copy) NSString *data;
@property(nonatomic, copy) void (^completion)(BOOL, NSString *);
@property(nonatomic) BOOL finished;
@property(nonatomic, strong) NSMutableIndexSet *visited;   /* steps whose defaults were applied */
@end
@implementation SKFomodController
- (SKFStep *)step { return self.fomod.steps[self.visible[self.position].unsignedIntegerValue]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.estimatedRowHeight = 70; self.tableView.rowHeight = UITableViewAutomaticDimension;
    [self refresh];
}
- (void)refresh {
    self.visible = self.fomod.visibleStepIndexes;
    if (self.position >= self.visible.count) self.position = self.visible.count ? self.visible.count - 1 : 0;
    BOOL last = self.position + 1 >= self.visible.count;
    self.title = self.visible.count ? self.step.name : self.fomod.title;
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:self.position ? T(@"Back", @"戻る") : T(@"Cancel", @"キャンセル")
        style:UIBarButtonItemStylePlain target:self action:@selector(back)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:last ? T(@"Install", @"インストール") : T(@"Next", @"次へ")
        style:UIBarButtonItemStyleDone target:self action:@selector(next)];
    [self.tableView reloadData];
}
- (void)finish:(BOOL)ok message:(NSString *)m {
    if (self.finished) return;
    self.finished = YES;
    void (^done)(BOOL, NSString *) = self.completion;
    [self dismissViewControllerAnimated:YES completion:^{ done(ok, m); }];
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
    return [NSString stringWithFormat:@"%@ — %@", g.name, rule[g.type] ?: g.type];
}
- (UITableViewCell *)tableView:(UITableView *)t cellForRowAtIndexPath:(NSIndexPath *)p {
    (void)t;
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    c.textLabel.numberOfLines = 2; c.detailTextLabel.numberOfLines = 6; c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    if (!self.visible.count) { c.textLabel.text = T(@"No choices needed. Tap Install.", @"選択する項目はありません。「インストール」を押してください。"); return c; }
    SKFOption *o = self.step.groups[p.section].options[p.row];
    NSString *type = [self.fomod typeOf:o flags:[self.fomod flagsBefore:self.visible[self.position].unsignedIntegerValue]];
    c.textLabel.text = [type isEqualToString:@"Recommended"] ? [o.name stringByAppendingString:T(@" (recommended)", @"（推奨）")] : o.name;
    c.detailTextLabel.text = o.desc;
    c.accessoryType = o.selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    BOOL locked = [type isEqualToString:@"Required"] || [type isEqualToString:@"NotUsable"] || [self.step.groups[p.section].type isEqualToString:@"SelectAll"];
    c.textLabel.enabled = ![type isEqualToString:@"NotUsable"];
    c.selectionStyle = locked ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
    return c;
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

void skyrim_fomod_run(NSString *raw, NSString *config, NSString *data, NSSet<NSString *> *active,
                      void (^completion)(BOOL, NSString *)) {
    SKFomod *f = [[SKFomod alloc] initWithConfig:config active:active raw:raw];
    if (!f) { completion(NO, T(@"The installer file could not be read.", @"インストーラーの設定ファイルを読めませんでした。")); return; }
    UIViewController *top = TopController();
    if (!top) { completion(NO, T(@"Open the app to run the installer.", @"インストーラーを表示できませんでした。")); return; }
    SKFomodController *c = [[SKFomodController alloc] initWithStyle:UITableViewStyleInsetGrouped];
    c.fomod = f; c.data = data; c.completion = completion;
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:c];
    nav.modalInPresentation = YES;
    [top presentViewController:nav animated:YES completion:nil];
}
