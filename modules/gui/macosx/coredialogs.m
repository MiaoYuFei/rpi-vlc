/*****************************************************************************
 * coredialogs.m: Mac OS X Core Dialogs
 *****************************************************************************
 * Copyright (C) 2005-2009 the VideoLAN team
 * $Id: 925c9949d2130a336f1801202573874e69ee9a61 $
 *
 * Authors: Derk-Jan Hartman <hartman at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#import "intf.h"
#import "coredialogs.h"
#import "misc.h"

/* for the icon in our custom error panel */
#import <ApplicationServices/ApplicationServices.h>


/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/

bool b_progress_cancelled;
static void updateProgressPanel (void *, const char *, float);
static bool checkProgressPanel (void *);
static void destroyProgressPanel (void *);

/*****************************************************************************
 * VLCCoreDialogProvider implementation
 *****************************************************************************/
@implementation VLCCoreDialogProvider

static VLCCoreDialogProvider *_o_sharedInstance = nil;

+ (VLCCoreDialogProvider *)sharedInstance
{
    return _o_sharedInstance ? _o_sharedInstance : [[self alloc] init];
}

-(id)init
{
    if( _o_sharedInstance )
        [self dealloc];
    else
    {
        _o_sharedInstance = [super init];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(performDialogEvent:)
                                                     name: @"VLCNewCoreDialogEventNotification"
                                                   object:self];
        [[NSNotificationCenter defaultCenter] addObserver: self
                                                 selector:@selector(performProgressBarEvent:)
                                                     name:@"VLCCoreDialogProgressBarUpdateNotification" 
                                                   object: self];
        [[NSNotificationCenter defaultCenter] addObserver: self
                                                 selector:@selector(performProgressBarEvent:)
                                                     name:@"VLCCoreDialogProgressBarDestroyNotification" 
                                                   object: self];
        o_error_panel = [[VLCErrorPanel alloc] init];
        b_progress_cancelled = NO;
    }
    
    return _o_sharedInstance;
}

-(void)awakeFromNib
{
    [o_auth_login_txt setStringValue: _NS("User name")];
    [o_auth_pw_txt setStringValue: _NS("Password")];
    [o_auth_cancel_btn setTitle: _NS("Cancel")];
    [o_auth_ok_btn setTitle: _NS("OK")];
    [o_prog_cancel_btn setTitle: _NS("Cancel")];
    [o_prog_bar setUsesThreadedAnimation: YES];

}    

-(void)performDialogEvent: (NSNotification *)o_notification
{
    NSValue *o_value = [[o_notification userInfo] objectForKey:@"VLCDialogPointer"];
    NSString *o_type = [[o_notification userInfo] objectForKey:@"VLCDialogType"];

    if( [o_type isEqualToString: @"dialog-fatal"] )
        [self showFatalDialog: o_value];
    else if( [o_type isEqualToString: @"dialog-question"] )
        [self showQuestionDialog: o_value];
    else if( [o_type isEqualToString: @"dialog-login"] )
        [self showLoginDialog: o_value];
    else if( [o_type isEqualToString: @"dialog-progress-bar"] )
        [self showProgressDialog: o_value];
    else
        msg_Err( VLCIntf, "unhandled dialog type: '%s'", [o_type UTF8String] );
}

-(void)showFatalDialog: (NSValue *)o_value
{
    dialog_fatal_t *p_dialog = [o_value pointerValue];
    /* do we need to block ? */
    if( p_dialog->modal == YES )
    {
        NSAlert *o_alert;
        o_alert = [NSAlert alertWithMessageText: [NSString stringWithUTF8String: p_dialog->title] defaultButton: _NS("OK") alternateButton: nil otherButton: nil informativeTextWithFormat: [NSString stringWithUTF8String: p_dialog->message]];
        [o_alert setAlertStyle: NSCriticalAlertStyle];
        [o_alert runModal];
    }
    else
    {
        [o_error_panel addError: [NSString stringWithUTF8String: p_dialog->title] withMsg: [NSString stringWithUTF8String: p_dialog->message]];
        [o_error_panel showPanel];
    }
}

-(void)showQuestionDialog: (NSValue *)o_value
{
    dialog_question_t *p_dialog = [o_value pointerValue];
    NSAlert *o_alert;
    NSString *o_yes, *o_no, *o_cancel;
    NSInteger i_returnValue = 0;
    
    if( p_dialog->yes != NULL )
        o_yes = [NSString stringWithUTF8String: p_dialog->yes];
    if( p_dialog->no != NULL )
        o_no = [NSString stringWithUTF8String: p_dialog->no];
    if( p_dialog->cancel != NULL )
        o_cancel = [NSString stringWithUTF8String: p_dialog->cancel];

    o_alert = [NSAlert alertWithMessageText: [NSString stringWithUTF8String: p_dialog->title] defaultButton: o_yes alternateButton:o_no otherButton: o_cancel informativeTextWithFormat: [NSString stringWithUTF8String: p_dialog->message]];
    [o_alert setAlertStyle: NSInformationalAlertStyle];
    i_returnValue = [o_alert runModal];

    if( i_returnValue == NSAlertDefaultReturn )
        p_dialog->answer = 1;
    if( i_returnValue == NSAlertAlternateReturn )
        p_dialog->answer = 2;
    if( i_returnValue == NSAlertOtherReturn )
        p_dialog->answer = 3;
}

-(void)showLoginDialog: (NSValue *)o_value
{
    dialog_login_t *p_dialog = [o_value pointerValue];
    NSInteger i_returnValue = 0;

    [o_auth_title_txt setStringValue: [NSString stringWithUTF8String: p_dialog->title]];
    [o_auth_win setTitle: [NSString stringWithUTF8String: p_dialog->title]];
    [o_auth_description_txt setStringValue: [NSString stringWithUTF8String: p_dialog->message]];
    [o_auth_login_fld setStringValue: @""];
    [o_auth_pw_fld setStringValue: @""];

    [o_auth_win center];
    i_returnValue = [NSApp runModalForWindow: o_auth_win];
    [o_auth_win close];
    if( i_returnValue )
    {
        *p_dialog->username = strdup( [[o_auth_login_fld stringValue] UTF8String] );
        *p_dialog->password = strdup( [[o_auth_pw_fld stringValue] UTF8String] );
    }
    else
    {
         *p_dialog->username = *p_dialog->password = NULL;
    }
}

-(IBAction)loginDialogAction:(id)sender
{
    if( [[sender title] isEqualToString: _NS("OK")] )
        [NSApp stopModalWithCode: 1];
    else
        [NSApp stopModalWithCode: 0];
}

-(void)showProgressDialog: (NSValue *)o_value
{
    dialog_progress_bar_t *p_dialog = [o_value pointerValue];

    if( p_dialog->title != NULL )
    {
        [o_prog_win setTitle: [NSString stringWithUTF8String: p_dialog->title]];
        [o_prog_title_txt setStringValue: [NSString stringWithUTF8String: p_dialog->title]];
    }
    else
    {
        [o_prog_win setTitle: @""];
        [o_prog_title_txt setStringValue: @""];
    }
    if( p_dialog->cancel != NULL )
        [o_prog_cancel_btn setTitle: [NSString stringWithUTF8String: p_dialog->cancel]];
    else
        [o_prog_cancel_btn setTitle: _NS("Cancel")];
    if( p_dialog->message != NULL )
        [o_prog_description_txt setStringValue: [NSString stringWithUTF8String: p_dialog->message]];
    else
        [o_prog_description_txt setStringValue: @""];
    [o_prog_bar setDoubleValue: 0];

    p_dialog->pf_update = updateProgressPanel;
    p_dialog->pf_check = checkProgressPanel;
    p_dialog->pf_destroy = destroyProgressPanel;
    p_dialog->p_sys = self;

    [NSApp runModalForWindow: o_prog_win];
}

-(void)performProgressBarEvent: (NSNotification *)o_notification
{
    NSLog( @"%@ received", [o_notification name] );
    if( [[o_notification name] isEqualToString: @"VLCCoreDialogProgressBarUpdateNotification"] )
    {
        NSNumber *o_number = [[o_notification userInfo] objectForKey:@"IntValue"];
        NSString *o_text = [[o_notification userInfo] objectForKey:@"Text"];
        [o_prog_description_txt setStringValue: o_text];
        [o_prog_bar setDoubleValue: [o_number doubleValue]];
    }
    if( [[o_notification name] isEqualToString: @"VLCCoreDialogProgressBarDestroyNotification"] )
    {
        [NSApp stopModalWithCode: 0];
        [o_prog_win close];
    }
}

void updateProgressPanel (void *priv, const char *text, float value)
{
    NSLog( @"we were updated with %s (%f)", text, value );
    NSString *o_txt;
    if( text != NULL )
        o_txt = [NSString stringWithUTF8String: text];
    [[NSNotificationCenter defaultCenter] postNotificationName: @"VLCCoreDialogProgressBarUpdateNotification" object:[[VLCMain sharedInstance] coreDialogProvider] userInfo:[NSDictionary dictionaryWithObjectsAndKeys: o_txt, @"Text", [NSNumber numberWithInt: ((int)(value * 1000.))], @"IntValue", nil]];
}

void destroyProgressPanel (void *priv)
{
    NSLog( @"we should destroy" );
    [[NSNotificationCenter defaultCenter] postNotificationName: @"VLCCoreDialogProgressBarDestroyNotification" object:[[VLCMain sharedInstance] coreDialogProvider] userInfo: nil];
}

bool checkProgressPanel (void *priv)
{
    NSLog( @"we were checked" );
    return b_progress_cancelled;
}

-(IBAction)progDialogAction:(id)sender
{
    NSLog( @"buttonAction!" );
    b_progress_cancelled = YES;
}

-(id)errorPanel
{
    return o_error_panel;
}

-(void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}
@end

/*****************************************************************************
 * VLCErrorPanel implementation
 *****************************************************************************/
@implementation VLCErrorPanel
-(id)init
{
    [super init];

    nib_loaded = [NSBundle loadNibNamed:@"InteractionErrorPanel" owner:self];

    /* init strings */
    [o_window setTitle: _NS("Errors and Warnings")];
    [o_cleanup_button setTitle: _NS("Clean up")];
    [o_messages_btn setTitle: _NS("Show Details")];

    /* init data sources */
    o_errors = [[NSMutableArray alloc] init];
    o_icons = [[NSMutableArray alloc] init];

    return self;
}

-(void)dealloc
{
    [o_errors release];
    [o_icons release];
    [super dealloc];
}

-(void)showPanel
{
    [o_window makeKeyAndOrderFront: self];
}

-(void)addError: (NSString *)o_error withMsg:(NSString *)o_msg
{
    /* format our string as desired */
    NSMutableAttributedString * ourError;
    ourError = [[NSMutableAttributedString alloc] initWithString:
        [NSString stringWithFormat:@"%@\n%@", o_error, o_msg]
        attributes:
        [NSDictionary dictionaryWithObject: [NSFont systemFontOfSize:11] forKey: NSFontAttributeName]];
    [ourError
        addAttribute: NSFontAttributeName
        value: [NSFont boldSystemFontOfSize:11]
        range: NSMakeRange( 0, [o_error length])];
    [o_errors addObject: ourError];
    [ourError release];

    [o_icons addObject: [NSImage imageWithErrorIcon]];

    [o_error_table reloadData];
}

-(IBAction)cleanupTable:(id)sender
{
    [o_errors removeAllObjects];
    [o_icons removeAllObjects];
    [o_error_table reloadData];
}

-(IBAction)showMessages:(id)sender
{
    [[VLCMain sharedInstance] showMessagesPanel: sender];
}

/*----------------------------------------------------------------------------
 * data source methods
 *---------------------------------------------------------------------------*/
- (NSInteger)numberOfRowsInTableView:(NSTableView *)theDataTable
{
    return [o_errors count];
}

- (id)tableView:(NSTableView *)theDataTable objectValueForTableColumn:
    (NSTableColumn *)theTableColumn row: (NSInteger)row
{
    if( [[theTableColumn identifier] isEqualToString: @"error_msg"] )
        return [o_errors objectAtIndex: row];

    if( [[theTableColumn identifier] isEqualToString: @"icon"] )
        return [o_icons objectAtIndex: row];

    return @"unknown identifier";
}

@end
