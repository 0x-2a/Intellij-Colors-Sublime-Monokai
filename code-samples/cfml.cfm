<cfparam name="instance" type="string" default="default">
<cfsetting enablecfoutputonly="true">

<cfscript>
// Handle service initialization if necessary
    oService = createObject("component", "bugLog.components.service").init(instanceName = instance);
    oAppService = createObject("component", "bugLog.components.hq.appService").init(instanceName = instance);

    oConfig = oService.getConfig();
    settings = oAppService.getDigestSettings();

    oMailerService = createObject("component", "bugLog.components.MailerService").init(oConfig);

    adminEmail = oConfig.getSetting("general.adminEmail");
    recipients = len(trim(settings.recipients)) ? trim(settings.recipients) : adminEmail;

    if (!settings.enabled) {
        writeOutput("Digest report is not enabled.");
        abort;
    } else
        if (!len(adminEmail) or !isValid("email", adminEmail)) {
            writeOutput("The Administrator Email setting must be a valid email address.");
            abort;
        }
</cfscript>

<cfsavecontent variable="tmpHTML">
    <cfoutput>
        <cfinclude template="renderDigest.cfm">
        <p style="font-family: arial,sans-serif;color:##666;font-size:11px;margin-top:20px;">
        ** This email has been sent from the BugLogHQ server at
            <a href="#thisHost#">#thisHost#</a>
    </p>
    </cfoutput>
</cfsavecontent>

<cfif qrydata.recordCount gt 0 or (qryData.recordCount eq 0 and settings.sendIfEmpty)>
    <cfset oMailerService.send(
        from = adminEmail,
        to = recipients,
        subject = "BugLogHQ Digest",
        body = tmpHTML,
        type = "html"
            )/>
    <cfoutput>Done. (email sent to #recipients#)</cfoutput>.
    <cfelse>
    <cfoutput>Done. (email not sent)</cfoutput>.
</cfif>
