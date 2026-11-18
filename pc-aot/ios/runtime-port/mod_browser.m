/* SPDX-License-Identifier: GPL-3.0-only
 * Nexus Mods (2011 edition) in a web view. The user signs in and presses the
 * site's own Manual Download button; the finished file is imported like a
 * shared file. No API keys, no automated downloads, no credentials stored
 * by the app (the web view keeps the site's own cookies). */
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "mod_install.h"
#include "mod_manager.h"

static NSString *T(NSString *english, NSString *japanese) {
    NSDictionary *s = [NSUserDefaults.standardUserDefaults dictionaryForKey:@"mobile-settings-v1"];
    return [s[@"language"] isKindOfClass:NSNumber.class] && [s[@"language"] intValue] == 1 ? japanese : english;
}

@interface SKModBrowser : UIViewController <WKNavigationDelegate, WKDownloadDelegate, WKUIDelegate>
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) UIProgressView *bar;
@property(nonatomic, strong) NSMutableDictionary<NSValue *, NSURL *> *destinations;
@end

@implementation SKModBrowser
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Nexus Mods";
    self.destinations = [NSMutableDictionary dictionary];
    WKWebViewConfiguration *config = [WKWebViewConfiguration new];
    config.websiteDataStore = WKWebsiteDataStore.defaultDataStore;     /* keeps the user's sign-in */
    self.web = [[WKWebView alloc] initWithFrame:self.view.bounds configuration:config];
    self.web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.web.navigationDelegate = self; self.web.UIDelegate = self;
    self.web.allowsBackForwardNavigationGestures = YES;
    [self.view addSubview:self.web];
    self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
    self.bar.frame = CGRectMake(0, 0, self.view.bounds.size.width, 3);
    self.bar.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    self.bar.hidden = YES;
    [self.view addSubview:self.bar];
    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise"] style:UIBarButtonItemStylePlain target:self.web action:@selector(reload)],
        [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"chevron.backward"] style:UIBarButtonItemStylePlain target:self.web action:@selector(goBack)]];
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://www.nexusmods.com/skyrim/mods/"]]];
    skyrim_mobile_show_notice(T(@"Use mods for \"Skyrim\" (not Special Edition). On a mod's Files tab choose Manual Download.",
        @"「Skyrim」（Special Editionではない方）のMODを選び、Filesタブの「Manual Download」を押すと取り込まれます。"));
}
- (BOOL)isArchive:(NSString *)name {
    NSString *ext = name.pathExtension.lowercaseString;
    return [@[@"zip", @"7z", @"rar"] containsObject:ext];
}
- (void)webView:(WKWebView *)w decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
    (void)w;
    NSString *scheme = action.request.URL.scheme.lowercaseString;
    if ([scheme isEqualToString:@"nxm"]) {
        skyrim_mobile_show_notice(T(@"\"Mod Manager Download\" needs a PC mod manager. Use \"Manual Download\" instead.",
            @"「Mod Manager Download」はPC用の管理ツール向けです。「Manual Download」を使ってください。"));
        decide(WKNavigationActionPolicyCancel); return;
    }
    if (![scheme isEqualToString:@"https"] && ![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"about"] && ![scheme isEqualToString:@"blob"]) {
        decide(WKNavigationActionPolicyCancel); return;
    }
    decide(action.shouldPerformDownload ? WKNavigationActionPolicyDownload : WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView *)w decidePolicyForNavigationResponse:(WKNavigationResponse *)response decisionHandler:(void (^)(WKNavigationResponsePolicy))decide {
    (void)w;
    NSHTTPURLResponse *http = [response.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response.response : nil;
    NSString *disposition = [http valueForHTTPHeaderField:@"Content-Disposition"] ?: @"";
    BOOL attachment = [disposition.lowercaseString containsString:@"attachment"];
    decide(!response.canShowMIMEType || attachment || [self isArchive:response.response.suggestedFilename ?: @""]
           ? WKNavigationResponsePolicyDownload : WKNavigationResponsePolicyAllow);
}
- (void)webView:(WKWebView *)w navigationAction:(WKNavigationAction *)a didBecomeDownload:(WKDownload *)d { (void)w; (void)a; d.delegate = self; }
- (void)webView:(WKWebView *)w navigationResponse:(WKNavigationResponse *)r didBecomeDownload:(WKDownload *)d { (void)w; (void)r; d.delegate = self; }
- (WKWebView *)webView:(WKWebView *)w createWebViewWithConfiguration:(WKWebViewConfiguration *)c forNavigationAction:(WKNavigationAction *)a windowFeatures:(WKWindowFeatures *)f {
    (void)c; (void)f;
    if (!a.targetFrame.isMainFrame) [w loadRequest:a.request];       /* open target=_blank in place */
    return nil;
}
- (void)download:(WKDownload *)download decideDestinationUsingResponse:(NSURLResponse *)response suggestedFilename:(NSString *)name completionHandler:(void (^)(NSURL *))done {
    (void)response;
    if (![self isArchive:name]) {
        skyrim_mobile_show_notice(T(@"Only zip, 7z and rar MOD files can be added.", @"追加できるのはzip・7z・rarのMODファイルだけです。"));
        done(nil); return;
    }
    NSURL *folder = [NSFileManager.defaultManager.temporaryDirectory URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:NULL];
    NSURL *target = [folder URLByAppendingPathComponent:name.lastPathComponent];
    self.destinations[[NSValue valueWithNonretainedObject:download]] = target;
    self.bar.hidden = NO; self.bar.observedProgress = download.progress;
    skyrim_mobile_show_notice([NSString stringWithFormat:T(@"Downloading %@…", @"%@ をダウンロード中…"), name]);
    done(target);
}
- (void)downloadDidFinish:(WKDownload *)download {
    NSValue *key = [NSValue valueWithNonretainedObject:download];
    NSURL *file = self.destinations[key];
    [self.destinations removeObjectForKey:key];
    self.bar.hidden = YES;
    if (file) {
        skyrim_mods_import_url(file, NO);            /* copies the file before returning */
        [NSFileManager.defaultManager removeItemAtURL:file.URLByDeletingLastPathComponent error:NULL];
    }
}
- (void)download:(WKDownload *)download didFailWithError:(NSError *)error resumeData:(NSData *)resume {
    (void)error; (void)resume;
    [self.destinations removeObjectForKey:[NSValue valueWithNonretainedObject:download]];
    self.bar.hidden = YES;
    skyrim_mobile_show_notice(T(@"The download failed. Try again.", @"ダウンロードに失敗しました。もう一度試してください。"));
}
@end

UIViewController *skyrim_mods_browser_controller(void) { return [SKModBrowser new]; }
