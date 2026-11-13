/* SPDX-License-Identifier: GPL-3.0-only
 * Native controls are outside the D3D9 render pass. No game-memory hooks,
 * guest addresses, process snapshots or original installation edits. */
#import <UIKit/UIKit.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "mobile_ui.h"
#include "mobile_core.h"
#include "ios_runtime.h"

static NSMutableDictionary *settings;
static NSDictionary *startup;
static NSString *profilePath;
static uint64_t frameCount;
static int renderWidth, renderHeight;
static unsigned iniCount;
static NSTimeInterval mobileTime(void) { return (double)skyrim_mobile_now()/1e9; }
static struct { const char *section, *key; char value[48]; } ini[40];
static double value(NSString *key, double fallback) {
    id number = settings[key];
    return [number isKindOfClass:NSNumber.class] && isfinite([number doubleValue]) ? [number doubleValue] : fallback;
}
static double clamp(double x, double lo, double hi) { return fmin(hi, fmax(lo, x)); }
static void persist(void) {
    [NSUserDefaults.standardUserDefaults setObject:settings forKey:@"mobile-settings-v1"];
    skyrim_mobile_set_limit((unsigned)value(@"fps", 0), value(@"thermal", 0) != 0);
}
static void addINI(const char *section, const char *key, double v) {
    if (iniCount >= sizeof ini / sizeof ini[0]) abort();
    ini[iniCount].section = section; ini[iniCount].key = key;
    snprintf(ini[iniCount].value, sizeof ini[iniCount].value, "%.4f", v);
    ++iniCount;
}
static void integerINI(const char *section, const char *key, double v) {
    addINI(section, key, v);
    snprintf(ini[iniCount - 1].value, sizeof ini[0].value, "%d", (int)v);
}
/* The explicit selection whitelist is shared with the settings screen. */
static NSArray<NSDictionary *> *graphicsRows(void) {
    return @[
      @{@"key":@"resolution", @"title":@"描画の高さ", @"values":@[@0,@360,@540,@720,@900,@1080,@2160], @"labels":@[@"維持",@"360p",@"540p",@"720p",@"900p",@"1080p",@"画面の実ピクセル数"]},
      @{@"key":@"texture", @"title":@"テクスチャ", @"values":@[@-1,@2,@1,@0], @"labels":@[@"維持",@"低",@"中",@"元の解像度"]},
      @{@"key":@"anisotropy", @"title":@"異方性フィルター", @"values":@[@-1,@1,@2,@4,@8,@16]},
      @{@"key":@"fxaa", @"title":@"FXAA", @"values":@[@-1,@0,@1], @"labels":@[@"維持",@"オフ",@"オン"]},
      @{@"key":@"msaa", @"title":@"MSAA", @"values":@[@-1,@0,@2,@4,@8], @"labels":@[@"維持",@"オフ",@"2×",@"4×",@"8×"]},
      @{@"key":@"shadow", @"title":@"影の解像度", @"values":@[@-1,@512,@1024,@2048,@4096]},
      @{@"key":@"shadowDistance", @"title":@"影の距離", @"values":@[@-1,@1500,@2000,@3000,@6000,@8000]},
      @{@"key":@"grass", @"title":@"草の表示距離", @"values":@[@-1,@0,@1000,@3000,@6000,@9000]},
      @{@"key":@"objects", @"title":@"物体の表示距離倍率", @"values":@[@-1,@2.5,@5,@8,@12,@15]},
      @{@"key":@"water", @"title":@"水面への風景反射", @"values":@[@-1,@0,@1], @"labels":@[@"維持",@"省略",@"表示"]},
      @{@"key":@"decals", @"title":@"血痕・着弾跡", @"values":@[@-1,@0,@1], @"labels":@[@"維持",@"オフ",@"オン"]}
    ];
}
void skyrim_mobile_initialize(const char *profile) {
    NSCAssert(NSThread.isMainThread, @"mobile startup must run on main");
    profilePath = [NSString stringWithUTF8String:profile];
    id saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    settings = [NSMutableDictionary dictionary];
    if ([saved isKindOfClass:NSDictionary.class])
        for (NSString *key in saved)
            if ([key isKindOfClass:NSString.class] && [saved[key] isKindOfClass:NSNumber.class] && isfinite([saved[key] doubleValue])) settings[key] = saved[key];
    for (NSDictionary *row in graphicsRows()) {
        NSNumber *choice = settings[row[@"key"]];
        if (choice && ![row[@"values"] containsObject:choice]) [settings removeObjectForKey:row[@"key"]];
    }
    if (![@[@0,@24,@30,@40,@60] containsObject:@(value(@"fps",0))]) [settings removeObjectForKey:@"fps"];
    persist(); startup = [settings copy];
    iniCount = 0;
    struct { const char *option, *section, *key; int integer; } map[] = {
        {"texture","Display","iTexMipMapSkip",1}, {"anisotropy","Display","iMaxAnisotropy",1},
        {"fxaa","Display","bFXAAEnabled",1}, {"msaa","Display","iMultiSample",1},
        {"shadow","Display","iShadowMapResolution",1}, {"shadowDistance","Display","fShadowDistance",0},
        {"grass","Grass","fGrassStartFadeDistance",0}, {"objects","LOD","fLODFadeOutMultObjects",0},
        {"objects","LOD","fLODFadeOutMultActors",0}, {"objects","LOD","fLODFadeOutMultItems",0},
        {"water","Water","bReflectLODLand",1}, {"water","Water","bReflectLODObjects",1},
        {"water","Water","bReflectLODTrees",1}, {"decals","Decals","bDecals",1},
        {"decals","Decals","bSkinnedDecals",1}
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; ++i) {
        NSNumber *v = startup[[NSString stringWithUTF8String:map[i].option]];
        if (!v || v.doubleValue < 0) continue;
        if (map[i].integer) integerINI(map[i].section,map[i].key,v.doubleValue);
        else addINI(map[i].section,map[i].key,v.doubleValue);
    }
    if (startup[@"texture"] && [startup[@"texture"] intValue] >= 0)
        integerINI("Display", "iTexMipMapMinimum", 0);
    if (startup[@"decals"] && [startup[@"decals"] intValue] >= 0) {
        int enabled = [startup[@"decals"] intValue] != 0;
        integerINI("Display","iMaxDecalsPerFrame",enabled ? 30 : 0);
        integerINI("Display","iMaxSkinDecalsPerFrame",enabled ? 10 : 0);
        integerINI("Decals","uMaxDecals",enabled ? 100 : 0);
        integerINI("Decals","uMaxSkinDecals",enabled ? 50 : 0);
        integerINI("Decals","uMaxSkinDecalPerActor",enabled ? 20 : 0);
    }
}
int skyrim_mobile_verbose(void) { return [startup[@"verbose"] boolValue]; }
void skyrim_mobile_render_size(int *width, int *height) {
    int selected = [startup[@"resolution"] intValue];
    if (!selected) return;
    int w = *width, h = *height;
    skyrim_ios_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    renderHeight = selected >= 2160 ? h : MIN(h, selected);
    renderHeight = MAX(2, renderHeight & ~1);
    renderWidth = MAX(2, ((int)lround((double)w * renderHeight / h)) & ~1);
    *width = renderWidth; *height = renderHeight;
}
int skyrim_mobile_graphics_changed(void) { return iniCount || [startup[@"resolution"] intValue]; }
int skyrim_mobile_ini(unsigned index, const char **section, const char **key, const char **out) {
    /* Called once at startup, before the translated entry and its threads. */
    static int initialized;
    if (!initialized) {
        initialized = 1;
        if (renderWidth && renderHeight) {
            integerINI("Display", "iSize W", renderWidth);
            integerINI("Display", "iSize H", renderHeight);
        }
        fprintf(stderr,"IOS_GRAPHICS override_count=%u resolution=%dx%d\n",iniCount,renderWidth,renderHeight);
    }
    if (index >= iniCount) return 0;
    *section = ini[index].section; *key = ini[index].key; *out = ini[index].value;
    return 1;
}
void skyrim_mobile_frame(void) { __atomic_add_fetch(&frameCount,1,__ATOMIC_RELAXED); }

@class SKOverlay;
static SKOverlay *overlay;
@interface SKPadControl : UIView
@property(nonatomic) NSInteger code;
@property(nonatomic,copy) NSString *caption;
@property(nonatomic,strong) UITouch *finger;
@property(nonatomic) BOOL editing;
@property(nonatomic) CGPoint stick;
@end
@interface SKOverlay : UIView
@property(nonatomic,strong) NSMutableArray<SKPadControl *> *controls;
@property(nonatomic,strong) NSMutableDictionary *positions;
@property(nonatomic,strong) UITouch *lookFinger;
@property(nonatomic) CGPoint lookPoint;
@property(nonatomic) NSTimeInterval lookTime;
@property(nonatomic) BOOL editing;
@property(nonatomic,strong) UILabel *hud;
@property(nonatomic,strong) UILabel *notice;
@property(nonatomic) NSTimeInterval noticeUntil;
@property(nonatomic,strong) UIButton *settingsButton;
@property(nonatomic,strong) UIButton *saveButton;
@property(nonatomic,strong) UIButton *keyboardButton;
@property(nonatomic,strong) NSDictionary *saveBaseline;
@property(nonatomic) NSTimeInterval saveDeadline;
- (void)clearInput;
- (void)message:(NSString *)text;
- (void)openSettings;
- (void)beginLayout;
- (void)endLayout;
@end

@implementation SKPadControl
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.clearColor; self.opaque=NO;
        self.multipleTouchEnabled=YES; self.exclusiveTouch=NO;
        self.isAccessibilityElement=YES;
    }
    return self;
}
- (void)drawRect:(CGRect)rect {
    CGRect circle=CGRectInset(self.bounds,2,2);
    UIBezierPath *path=[UIBezierPath bezierPathWithRoundedRect:circle cornerRadius:self.code==18 ? self.bounds.size.width/2 : 14];
    [[UIColor colorWithWhite:self.finger ? 0.45 : 0.08 alpha:0.78] setFill]; [path fill];
    [[UIColor colorWithWhite:1 alpha:self.editing ? 1 : 0.65] setStroke]; path.lineWidth=self.editing ? 2.5 : 1; [path stroke];
    if (self.code==18) {
        CGFloat size=self.bounds.size.width*0.4;
        CGPoint center=CGPointMake(CGRectGetMidX(self.bounds)+self.stick.x*self.bounds.size.width*.28,
                                  CGRectGetMidY(self.bounds)-self.stick.y*self.bounds.size.height*.28);
        [[UIColor colorWithWhite:0.85 alpha:0.65] setFill];
        [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(center.x-size/2,center.y-size/2,size,size)] fill];
    } else {
        NSMutableParagraphStyle *style=[NSMutableParagraphStyle new]; style.alignment=NSTextAlignmentCenter;
        NSDictionary *attrs=@{NSFontAttributeName:[UIFont systemFontOfSize:MAX(9,MIN(13,self.bounds.size.height*.23)) weight:UIFontWeightSemibold],NSForegroundColorAttributeName:UIColor.whiteColor,NSParagraphStyleAttributeName:style};
        CGSize text=[self.caption boundingRectWithSize:CGSizeMake(self.bounds.size.width-4,100) options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs context:nil].size;
        [self.caption drawInRect:CGRectMake(2,(self.bounds.size.height-text.height)/2,self.bounds.size.width-4,text.height) withAttributes:attrs];
    }
}
- (void)updateStick:(UITouch *)touch {
    CGPoint p=[touch locationInView:self]; CGFloat radius=self.bounds.size.width*.36;
    float x=(p.x-CGRectGetMidX(self.bounds))/radius, y=(CGRectGetMidY(self.bounds)-p.y)/radius;
    float length=hypotf(x,y); if (length>1) {x/=length;y/=length;}
    self.stick=CGPointMake(x,y); skyrim_mobile_move(x,y,clamp(value(@"deadzone",.10),0,.3));
    [self setNeedsDisplay];
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || self.finger) return;
    self.finger=touches.anyObject;
    if (self.code==18) [self updateStick:self.finger]; else skyrim_mobile_button((unsigned)self.code,1);
    [self setNeedsDisplay];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.editing && self.code==18 && self.finger && [touches containsObject:self.finger]) [self updateStick:self.finger];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.finger || ![touches containsObject:self.finger]) return;
    self.finger=nil;
    if (self.code==18) {self.stick=CGPointZero;skyrim_mobile_move(0,0,0);} else skyrim_mobile_button((unsigned)self.code,0);
    [self setNeedsDisplay];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}
@end

@interface SKSettings : UITableViewController
@end
static NSArray *presets(void) { return @[@"最軽量",@"パフォーマンス",@"バランス",@"高画質",@"最高画質"]; }
static NSString *choiceLabel(NSDictionary *row, NSNumber *choice) {
    NSUInteger i=[row[@"values"] indexOfObject:choice];
    if (i!=NSNotFound && row[@"labels"]) return row[@"labels"][i];
    if (choice.doubleValue < 0) return @"維持";
    return choice.stringValue;
}
@implementation SKSettings
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"SkyrimIOS 設定";
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"完了" style:UIBarButtonItemStyleDone target:self action:@selector(close)];
}
- (void)close { [overlay clearInput]; [self dismissViewControllerAnimated:YES completion:nil]; }
- (NSInteger)numberOfSectionsInTableView:(UITableView *)table { return 4; }
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section {
    return section==0 ? 4 : section==1 ? 5 : section==2 ? graphicsRows().count+1 : 2;
}
- (NSString *)tableView:(UITableView *)table titleForHeaderInSection:(NSInteger)section {
    return @[@"フレームレートと発熱",@"タッチ操作",@"画質 — 次回起動から反映",@"ボタン配置"][section];
}
- (NSString *)tableView:(UITableView *)table titleForFooterInSection:(NSInteger)section {
    if (section==0) return @"FPSは上限です。発熱を抑えるなら30を目安に。自動抑制はiOSが高温を報告した時に30／24へ上限を下げます。詳細ログは次回起動から反映。";
    if (section==1) return @"ボタン以外をドラッグして視点移動。十字キーとLB／RBでメニュー移動、LT／RTで左右の手を操作します。";
    if (section==2) return @"変更後はゲーム内で保存し、アプリを完全終了して開き直してください。「維持」は以降の上書きを止める選択で、以前の値には戻しません。最高画質でも滑らかに動く保証はありません。";
    if (section==3) return @"設定画面だけではゲームは一時停止しません。先にシステムメニューを開いてください。クイックセーブはゲームのF5入力を送り、保存ファイルの更新を確認します。実行状態を丸ごと復元するステートセーブではありません。";
    return nil;
}
- (NSDictionary *)row:(NSIndexPath *)p {
    if (p.section==0) return @[
       @{@"key":@"fps",@"title":@"FPS上限",@"values":@[@0,@24,@30,@40,@60],@"labels":@[@"追加制限なし",@"24",@"30",@"40",@"60"]},
       @{@"key":@"thermal",@"title":@"高温時の自動抑制",@"values":@[@0,@1],@"labels":@[@"オフ",@"オン"]},
       @{@"key":@"hud",@"title":@"FPS・発熱状態の表示",@"values":@[@0,@1],@"labels":@[@"オフ",@"オン"],@"default":@1},
       @{@"key":@"verbose",@"title":@"詳細診断ログ",@"values":@[@0,@1],@"labels":@[@"オフ（通常）",@"オン（調査用）"]}
    ][p.row];
    if (p.section==1) return @[
       @{@"key":@"sensitivity",@"title":@"視点感度",@"values":@[@.25,@.5,@.75,@1,@1.5,@2,@3,@4],@"default":@1.5},
       @{@"key":@"invert",@"title":@"視点の上下反転",@"values":@[@0,@1],@"labels":@[@"オフ",@"オン"]},
       @{@"key":@"deadzone",@"title":@"移動スティックの遊び",@"values":@[@0,@.05,@.10,@.15,@.2,@.3],@"default":@.1},
       @{@"key":@"size",@"title":@"ボタンの大きさ",@"values":@[@.8,@1,@1.2,@1.4],@"default":@1},
       @{@"key":@"opacity",@"title":@"ボタンの不透明度",@"values":@[@.25,@.4,@.6,@.8,@1],@"default":@.8}
    ][p.row];
    if (p.section==2 && p.row) return graphicsRows()[p.row-1];
    return nil;
}
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)p {
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    NSDictionary *r=[self row:p];
    if (r) {
        cell.textLabel.text=r[@"title"];
        NSNumber *choice=settings[r[@"key"]] ?: r[@"default"] ?: (p.section==2 ? [r[@"values"] firstObject] : @0);
        cell.detailTextLabel.text=choiceLabel(r,choice);
    } else if(p.section==2) {cell.textLabel.text=@"画質プリセット";cell.detailTextLabel.text=@"選択";}
    else cell.textLabel.text=p.row ? @"配置を初期状態に戻す" : @"ボタンをドラッグして配置";
    cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)applyPreset:(NSUInteger)p {
    /* Each tuple uses only existing SkyrimPrefs keys; no shader or game-code patch. */
    const double v[5][11]={{360,2,1,0,0,512,1500,0,2.5,0,0},
        {540,1,2,1,0,1024,2000,1000,5,0,1},{720,0,4,1,0,1024,3000,3000,8,1,1},
        {900,0,8,1,2,2048,6000,6000,12,1,1},{2160,0,16,0,4,4096,8000,9000,15,1,1}};
    NSArray *rows=graphicsRows();
    for (NSUInteger i=0;i<rows.count;++i) settings[rows[i][@"key"]]=@(v[p][i]);
    persist(); [self.tableView reloadData]; [overlay message:@"画質を保存しました。次回起動から反映します"];
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)p {
    [table deselectRowAtIndexPath:p animated:YES];
    if (p.section==3 && !p.row) {
        [self dismissViewControllerAnimated:YES completion:^{[overlay beginLayout];}]; return;
    }
    UIAlertController *sheet=[UIAlertController alertControllerWithTitle:p.section==3 ? @"ボタン配置を戻しますか？" : (p.section==2 && !p.row ? @"画質プリセット" : [self row:p][@"title"]) message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    if (p.section==3) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"配置をリセット" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action){
            [overlay.positions removeAllObjects]; [NSUserDefaults.standardUserDefaults removeObjectForKey:@"mobile-layout-v1"]; [overlay setNeedsLayout];
        }]];
    } else if (p.section==2 && !p.row) {
        for (NSUInteger i=0;i<presets().count;++i) [sheet addAction:[UIAlertAction actionWithTitle:presets()[i] style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){[self applyPreset:i];}]];
    } else {
        NSDictionary *row=[self row:p];
        for (NSNumber *choice in row[@"values"]) [sheet addAction:[UIAlertAction actionWithTitle:choiceLabel(row,choice) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
            settings[row[@"key"]]=choice; persist(); [overlay setNeedsLayout]; [table reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"キャンセル" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView=[table cellForRowAtIndexPath:p];
    sheet.popoverPresentationController.sourceRect=[table cellForRowAtIndexPath:p].bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}
@end

@implementation SKOverlay
- (UIButton *)utility:(NSString *)title action:(SEL)action {
    UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal]; [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    b.titleLabel.font=[UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
    b.backgroundColor=[UIColor colorWithWhite:.1 alpha:.75]; b.layer.cornerRadius=10;
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside]; [self addSubview:b]; return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.clearColor; self.opaque=NO; self.multipleTouchEnabled=YES;
        self.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        self.controls=[NSMutableArray array]; self.positions=[NSMutableDictionary dictionary];
        id saved=[NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-layout-v1"];
        if ([saved isKindOfClass:NSDictionary.class]) [self.positions addEntriesFromDictionary:saved];
        NSArray *captions=@[@"↑",@"↓",@"←",@"→",@"システム\nSTART",@"待機\nBACK",@"隠密\nL3",@"視点\nR3",@"走る\nLB",@"シャウト\nRB",@"",@"",@"使う／決定\nA",@"メニュー／戻る\nB",@"武器\nX",@"ジャンプ\nY",@"左手／防御\nLT",@"右手／攻撃\nRT",@"移動"];
        for (NSInteger i=0;i<captions.count;++i) {
            if (i==10 || i==11) continue;
            SKPadControl *c=[[SKPadControl alloc] initWithFrame:CGRectZero]; c.code=i; c.caption=captions[i]; c.accessibilityLabel=[captions[i] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
            UIPanGestureRecognizer *pan=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(moveControl:)]; pan.enabled=NO; [c addGestureRecognizer:pan];
            [self.controls addObject:c]; [self addSubview:c];
        }
        self.settingsButton=[self utility:@"設定" action:@selector(openSettings)];
        self.saveButton=[self utility:@"クイック保存" action:@selector(quicksave)];
        self.keyboardButton=[self utility:@"文字入力" action:@selector(keyboard)];
        self.hud=[UILabel new]; self.hud.font=[UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightMedium];
        self.hud.textColor=UIColor.whiteColor; self.hud.backgroundColor=[UIColor colorWithWhite:0 alpha:.5]; self.hud.layer.cornerRadius=5; self.hud.clipsToBounds=YES; [self addSubview:self.hud];
        self.notice=[UILabel new]; self.notice.font=[UIFont systemFontOfSize:13 weight:UIFontWeightMedium]; self.notice.textColor=UIColor.whiteColor;
        self.notice.backgroundColor=[UIColor colorWithWhite:0 alpha:.8]; self.notice.textAlignment=NSTextAlignmentCenter; self.notice.numberOfLines=2; self.notice.hidden=YES; [self addSubview:self.notice];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(resignActive:) name:UIApplicationWillResignActiveNotification object:nil];
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    static const float xy[19][2]={{.34,.60},{.34,.80},{.29,.70},{.39,.70},{.62,.07},{.48,.07},{.34,.94},{.70,.94},{.12,.36},{.75,.36},{0,0},{0,0},{.87,.79},{.94,.61},{.80,.61},{.87,.43},{.77,.19},{.93,.19},{.13,.73}};
    CGRect area=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    CGFloat base=clamp(area.size.height*.13,40,58)*clamp(value(@"size",1),.8,1.4);
    for (SKPadControl *c in self.controls) {
        CGFloat x=xy[c.code][0],y=xy[c.code][1]; id point=self.positions[@(c.code).stringValue];
        if ([point isKindOfClass:NSArray.class] && [point count]==2 && [point[0] isKindOfClass:NSNumber.class] && [point[1] isKindOfClass:NSNumber.class] && isfinite([point[0] doubleValue]) && isfinite([point[1] doubleValue])) {x=[point[0] doubleValue];y=[point[1] doubleValue];}
        CGFloat w=c.code==18 ? base*2.2 : c.code==4 || c.code==5 ? base*1.15 : base;
        CGFloat h=c.code==18 ? w : base;
        CGFloat cx=clamp(area.origin.x+x*area.size.width,area.origin.x+w/2,CGRectGetMaxX(area)-w/2);
        CGFloat cy=clamp(area.origin.y+y*area.size.height,area.origin.y+h/2,CGRectGetMaxY(area)-h/2);
        c.frame=CGRectMake(cx-w/2,cy-h/2,w,h); c.alpha=self.editing ? 1 : clamp(value(@"opacity",.8),.25,1); [c setNeedsDisplay];
    }
    CGFloat y=area.origin.y+5;
    self.saveButton.frame=CGRectMake(area.origin.x+6,y,96,36);
    self.keyboardButton.frame=CGRectMake(area.origin.x+108,y,76,36);
    self.settingsButton.frame=CGRectMake(CGRectGetMaxX(area)-70,y,66,36);
    self.hud.frame=CGRectMake(area.origin.x+6,y+42,210,22);
    self.notice.frame=CGRectMake(area.origin.x+area.size.width*.22,CGRectGetMaxY(area)-64,area.size.width*.56,52);
}
- (void)message:(NSString *)text {self.notice.text=text;self.noticeUntil=mobileTime()+7;self.notice.hidden=NO;}
- (void)resignActive:(NSNotification *)note { [self clearInput]; }
- (void)clearInput {
    self.lookFinger=nil;
    for (SKPadControl *c in self.controls) {c.finger=nil;c.stick=CGPointZero;[c setNeedsDisplay];}
    skyrim_mobile_clear();
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || self.lookFinger) return;
    self.lookFinger=touches.anyObject;self.lookPoint=[self.lookFinger locationInView:self];self.lookTime=self.lookFinger.timestamp;
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.editing || !self.lookFinger || ![touches containsObject:self.lookFinger]) return;
    CGPoint p=[self.lookFinger locationInView:self]; NSTimeInterval t=self.lookFinger.timestamp;
    skyrim_mobile_look(p.x-self.lookPoint.x,p.y-self.lookPoint.y,t-self.lookTime,self.bounds.size.height,clamp(value(@"sensitivity",1.5),.25,4),value(@"invert",0)!=0);
    self.lookPoint=p;self.lookTime=t;
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (self.lookFinger && [touches containsObject:self.lookFinger]) {self.lookFinger=nil;skyrim_mobile_look_end();}
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {[self touchesEnded:touches withEvent:event];}
- (void)openSettings {
    [self clearInput];
    if(self.editing) {[self endLayout];return;}
    UIViewController *root=self.window.rootViewController;
    if(root.presentedViewController) return;
    SKSettings *controller=[[SKSettings alloc] initWithStyle:UITableViewStyleInsetGrouped];
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:controller];
    nav.modalPresentationStyle=UIModalPresentationFormSheet; nav.modalInPresentation=YES;
    [root presentViewController:nav animated:YES completion:nil];
}
- (void)beginLayout {
    [self clearInput]; self.editing=YES;
    for(SKPadControl *c in self.controls) {c.editing=YES;c.gestureRecognizers.firstObject.enabled=YES;}
    [self.settingsButton setTitle:@"配置完了" forState:UIControlStateNormal];
    self.saveButton.enabled=self.keyboardButton.enabled=NO;
    [self message:@"ボタンと移動スティックをドラッグ。右上の「配置完了」で保存"];
    [self setNeedsLayout];
}
- (void)endLayout {
    self.editing=NO;[self clearInput];
    for(SKPadControl *c in self.controls) {c.editing=NO;c.gestureRecognizers.firstObject.enabled=NO;}
    [NSUserDefaults.standardUserDefaults setObject:self.positions forKey:@"mobile-layout-v1"];
    [self.settingsButton setTitle:@"設定" forState:UIControlStateNormal];self.saveButton.enabled=self.keyboardButton.enabled=YES;
    [self message:@"ボタン配置を保存しました"];[self setNeedsLayout];
}
- (void)moveControl:(UIPanGestureRecognizer *)pan {
    if(!self.editing) return;
    UIView *c=pan.view; CGPoint d=[pan translationInView:self];[pan setTranslation:CGPointZero inView:self];
    CGRect area=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    if(area.size.width<1 || area.size.height<1) return;
    CGPoint center=CGPointMake(clamp(c.center.x+d.x,area.origin.x+c.bounds.size.width/2,CGRectGetMaxX(area)-c.bounds.size.width/2),clamp(c.center.y+d.y,area.origin.y+c.bounds.size.height/2,CGRectGetMaxY(area)-c.bounds.size.height/2));
    c.center=center; self.positions[@(((SKPadControl *)c).code).stringValue]=@[@((center.x-area.origin.x)/area.size.width),@((center.y-area.origin.y)/area.size.height)];
}
- (void)keyboard { [self clearInput];extern int platform_text_input(int);platform_text_input(1); }
- (NSDictionary *)saveFiles {
    NSString *path=[profilePath stringByAppendingPathComponent:@"Documents/My Games/Skyrim/Saves"];
    NSMutableDictionary *found=[NSMutableDictionary dictionary];
    for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:path error:NULL]) {
        if(![name.pathExtension.lowercaseString isEqualToString:@"ess"] || ![name.lowercaseString containsString:@"quicksave"]) continue;
        NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:[path stringByAppendingPathComponent:name] error:NULL];
        if([attrs[NSFileType] isEqual:NSFileTypeRegular]) found[name]=@[attrs[NSFileModificationDate],attrs[NSFileSize]];
    }
    return found;
}
- (void)quicksave {
    [self clearInput];self.saveBaseline=[self saveFiles];self.saveDeadline=mobileTime()+12;
    skyrim_mobile_quicksave_key();[self message:@"クイックセーブ（F5）を送信。保存ファイルの更新を確認中"];
}
@end

void skyrim_mobile_install(void *window) {
    NSCAssert(NSThread.isMainThread,@"UIKit overlay must run on main");
    SDL_SysWMinfo info;SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(window,&info) || info.subsystem!=SDL_SYSWM_UIKIT) {
        fprintf(stderr,"FATAL Cannot attach native touch controls: %s\n",SDL_GetError());abort();
    }
    UIWindow *native=info.info.uikit.window;
    UIView *root=native.rootViewController.view;
    if(!root) {fprintf(stderr,"FATAL SDL UIKit root view missing\n");abort();}
    overlay=[[SKOverlay alloc] initWithFrame:root.bounds];[root addSubview:overlay];
    [overlay message:@"空いている場所をドラッグして視点移動。右上の設定で感度・配置・画質を変更"];
}
void skyrim_mobile_tick(void) {
    if(!overlay || !NSThread.isMainThread) return;
    @autoreleasepool {
    /* SDL can add its Metal view after window creation. Restore overlay order
     * only then, not a D3D state block/capture/draw for every game frame. */
    if(overlay.superview.subviews.lastObject!=overlay) [overlay.superview bringSubviewToFront:overlay];
    static uint64_t lastFrames,logFrames;static NSTimeInterval lastTime,lastLog,lastSaveCheck;
    NSTimeInterval now=mobileTime();
    if(now-lastTime<.5) return;
    uint64_t frames=__atomic_load_n(&frameCount,__ATOMIC_RELAXED);
    double fps=lastTime ? (frames-lastFrames)/(now-lastTime) : 0;
    lastFrames=frames;lastTime=now;
    NSProcessInfoThermalState thermal=NSProcessInfo.processInfo.thermalState;
    skyrim_mobile_set_thermal((unsigned)thermal);
    NSArray *names=@[@"通常",@"温かい",@"高温",@"非常に高温"];
    unsigned cap=skyrim_mobile_fps_limit();
    overlay.hud.hidden=value(@"hud",1)==0;
    overlay.hud.text=[NSString stringWithFormat:@" %.1f FPS  %@  上限%@",fps,names[MIN((NSUInteger)thermal,3)],cap ? @(cap).stringValue : @"—"];
    overlay.notice.hidden=now>overlay.noticeUntil;
    if(now-lastLog>=5) {
        uint64_t count,total,maximum,interval;
        skyrim_ios_take_frame_metrics(&count,&total,&maximum,&interval);
        double average=lastLog ? (frames-logFrames)/(now-lastLog) : 0;
        fprintf(stderr,"IOS_PERF frames=%llu fps=%.2f cap=%u thermal=%ld present_host_avg_ms=%.2f present_host_max_ms=%.2f frame_gap_max_ms=%.2f\n",(unsigned long long)frames,average,cap,(long)thermal,count ? (double)total/count/1e6 : 0,(double)maximum/1e6,(double)interval/1e6);lastLog=now;logFrames=frames;
    }
    if(overlay.saveDeadline && now-lastSaveCheck>=1) {
        NSDictionary *current=[overlay saveFiles];lastSaveCheck=now;
        BOOL updated=NO;
        for(NSString *name in current)
            if([current[name][1] unsignedLongLongValue]>0 && ![current[name] isEqual:overlay.saveBaseline[name]]) updated=YES;
        if(updated) {
            [overlay message:@"クイックセーブのファイル更新を確認しました"];overlay.saveDeadline=0;
        } else if(now>=overlay.saveDeadline) {
            [overlay message:@"保存は未確認です。システム → SYSTEM → SAVEから保存してください"];overlay.saveDeadline=0;
        }
    }
    }
}
