/*
  Copyright 2024 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/


#include <generic_agent.h>

#include <bootstrap.h>
#include <policy_server.h>
#include <sysinfo.h>
#include <known_dirs.h>
#include <eval_context.h>
#include <policy.h>
#include <promises.h>
#include <file_lib.h> // FileCanOpen()
#include <files_lib.h>
#include <files_names.h>
#include <files_interfaces.h>
#include <hash.h>
#include <parser.h>
#include <dbm_api.h>
#include <crypto.h>
#include <vars.h>
#include <syntax.h>
#include <conversion.h>
#include <expand.h>
#include <locks.h>
#include <scope.h>
#include <cleanup.h>
#include <unix.h>
#include <client_code.h>
#include <string_lib.h> // StringCopy()
#include <regex.h>      // CompileRegex()
#include <writer.h>
#include <exec_tools.h>
#include <list.h>
#include <misc_lib.h>
#include <fncall.h>
#include <rlist.h>
#include <syslog_client.h>
#include <audit.h>
#include <verify_classes.h>
#include <verify_vars.h>
#include <timeout.h>
#include <time_classes.h>
#include <constants.h>
#include <ornaments.h>
#include <cf-windows-functions.h>
#include <loading.h>
#include <signals.h>
#include <addr_lib.h>
#include <openssl/evp.h>
#include <libgen.h>
#include <cleanup.h>
#include <cmdb.h>               /* LoadCMDBData() */
#include "cf3.defs.h"

#define AUGMENTS_VARIABLES_TAGS "tags"
#define AUGMENTS_VARIABLES_DATA "value"
#define AUGMENTS_CLASSES_TAGS "tags"
#define AUGMENTS_CLASSES_CLASS_EXPRESSIONS "class_expressions"
#define AUGMENTS_CLASSES_REGULAR_EXPRESSIONS "regular_expressions"
#define AUGMENTS_COMMENT_KEY "comment"

static pthread_once_t pid_cleanup_once = PTHREAD_ONCE_INIT; /* GLOBAL_T */

static char PIDFILE[CF_BUFSIZE] = ""; /* GLOBAL_C */

/* Used for 'ident' argument to openlog() */
static char CF_PROGRAM_NAME[256] = "";

static void CheckWorkingDirectories(EvalContext *ctx);

static void GetAutotagDir(char *dirname, size_t max_size, const char *maybe_dirname);
static void GetPromisesValidatedFile(char *filename, size_t max_size, const GenericAgentConfig *config, const char *maybe_dirname);
static bool WriteReleaseIdFile(const char *filename, const char *dirname);
static bool GeneratePolicyReleaseIDFromGit(char *release_id_out, size_t out_size,
                                           const char *policy_dir);
static bool GeneratePolicyReleaseID(char *release_id_out, size_t out_size,
                                    const char *policy_dir);
static char* ReadReleaseIdFromReleaseIdFileMasterfiles(const char *maybe_dirname);

static bool MissingInputFile(const char *input_file);

static bool LoadAugmentsFiles(EvalContext *ctx, const char* filename);

static void GetChangesChrootDir(char *buf, size_t buf_size);
static void DeleteChangesChroot();
static int ParseFacility(const char *name);
static inline const char *LogFacilityToString(int facility);

#if !defined(__MINGW32__)
static void OpenLog(int facility);
#endif

/*****************************************************************************/

static void SanitizeEnvironment()
{
    /* ps(1) and other utilities invoked by CFEngine may be affected */
    unsetenv("COLUMNS");

    /* Make sure subprocesses output is not localized */
    unsetenv("LANG");
    unsetenv("LANGUAGE");
    unsetenv("LC_MESSAGES");
}

/*****************************************************************************/

ENTERPRISE_VOID_FUNC_2ARG_DEFINE_STUB(void, GenericAgentSetDefaultDigest, HashMethod *, digest, int *, digest_len)
{
    *digest = HASH_METHOD_MD5;
    *digest_len = CF_MD5_LEN;
}

void SetCFEngineRoles(EvalContext *ctx)
{
    char cf_hub_path[PATH_MAX];
    snprintf(cf_hub_path, sizeof(cf_hub_path), "%s%ccf-hub", GetBinDir(), FILE_SEPARATOR);

    const bool is_reporting_hub = (access(cf_hub_path, F_OK) == 0);
    const bool is_policy_server = EvalContextClassGet(ctx, "default", "policy_server");

    if (!is_policy_server && !is_reporting_hub)
    {
        EvalContextClassPutHard(ctx, "cfengine_client", "report");
        Rlist *const roles = RlistFromSplitString("Client", ',');
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cfengine_roles", roles, CF_DATA_TYPE_STRING_LIST, "inventory,attribute_name=CFEngine roles");
        RlistDestroy(roles);
        return;
    }

    if (is_reporting_hub)
    {
        EvalContextClassPutHard(ctx, "cfengine_reporting_hub", "report");
    }

    // Community policy server:
    if (is_policy_server && !is_reporting_hub)
    {
        Rlist *const roles = RlistFromSplitString("Policy server", ',');
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cfengine_roles", roles, CF_DATA_TYPE_STRING_LIST, "inventory,attribute_name=CFEngine roles");
        RlistDestroy(roles);
        return;
    }

    // Enterprise hub bootstrapped to another policy server:
    // TODO: This is a weird situation, should we print a warning?
    if (is_reporting_hub && !is_policy_server)
    {
        Rlist *const roles = RlistFromSplitString("Reporting hub", ',');
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cfengine_roles", roles, CF_DATA_TYPE_STRING_LIST, "inventory,attribute_name=CFEngine roles");
        RlistDestroy(roles);
        return;
    }

    // Normal Enterprise hub:
    assert(is_policy_server && is_reporting_hub);
    Rlist *const roles = RlistFromSplitString("Policy server,Reporting hub", ',');
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cfengine_roles", roles, CF_DATA_TYPE_STRING_LIST, "inventory,attribute_name=CFEngine roles");
    RlistDestroy(roles);
    return;
}

void MarkAsPolicyServer(EvalContext *ctx)
{
    EvalContextClassPutHard(ctx, "am_policy_hub",
                            "source=bootstrap,deprecated,alias=policy_server");
    Log(LOG_LEVEL_VERBOSE, "Additional class defined: am_policy_hub");
    EvalContextClassPutHard(ctx, "policy_server", "report");
    Log(LOG_LEVEL_VERBOSE, "Additional class defined: policy_server");
}

Policy *SelectAndLoadPolicy(GenericAgentConfig *config, EvalContext *ctx, bool validate_policy, bool write_validated_file)
{
    Policy *policy = NULL;

    if (GenericAgentCheckPolicy(config, validate_policy, write_validated_file))
    {
        policy = LoadPolicy(ctx, config);
    }
    else if (config->tty_interactive)
    {
        Log(LOG_LEVEL_ERR,
               "Failsafe condition triggered. Interactive session detected, skipping failsafe.cf execution.");
    }
    else
    {
        Log(LOG_LEVEL_ERR, "CFEngine was not able to get confirmation of promises from cf-promises, so going to failsafe");
        EvalContextClassPutHard(ctx, "failsafe_fallback", "report,attribute_name=Errors,source=agent");

        if (CheckAndGenerateFailsafe(GetInputDir(), "failsafe.cf"))
        {
            GenericAgentConfigSetInputFile(config, GetInputDir(), "failsafe.cf");
            Log(LOG_LEVEL_ERR, "CFEngine failsafe.cf: %s %s", config->input_dir, config->input_file);
            policy = LoadPolicy(ctx, config);

            /* Doing failsafe, set the release_id to "failsafe" and also
             * overwrite the cfe_release_id file so that sub-agent executed as
             * part of failsafe can just pick it up and then rewrite it with the
             * actual value from masterfiles. */
            free(policy->release_id);
            policy->release_id = xstrdup("failsafe");

            char filename[PATH_MAX];
            GetReleaseIdFile(GetInputDir(), filename, sizeof(filename));
            FILE *release_id_stream = safe_fopen_create_perms(filename, "w",
                                                              CF_PERMS_DEFAULT);
            if (release_id_stream == NULL)
            {
                Log(LOG_LEVEL_ERR, "Failed to open the release_id file for writing during failsafe");
            }
            else
            {
                Writer *release_id_writer = FileWriter(release_id_stream);
                WriterWrite(release_id_writer, "{ releaseId: \"failsafe\" }\n");
                WriterClose(release_id_writer);
            }
        }
    }
    return policy;
}

static bool CheckContextClassmatch(EvalContext *ctx, const char *class_str)
{
    if (StringEndsWith(class_str, "::")) // Treat as class expression, not regex
    {
        const size_t length = strlen(class_str);
        if (length <= 2)
        {
            assert(length == 2); // True because StringEndsWith
            Log(LOG_LEVEL_ERR,
                "Invalid class expression in augments: '%s'",
                class_str);
            return false;
        }

        char *const tmp_class_str = xstrdup(class_str);
        assert(strlen(tmp_class_str) == length);

        tmp_class_str[length - 2] = '\0'; // 2 = strlen("::")
        const bool found = IsDefinedClass(ctx, tmp_class_str);

        free(tmp_class_str);
        return found;
    }

    ClassTableIterator *iter = EvalContextClassTableIteratorNewGlobal(ctx, NULL, true, true);
    StringSet *global_matches = ClassesMatching(ctx, iter, class_str, NULL, true); // returns early

    const bool found = (StringSetSize(global_matches) > 0);

    StringSetDestroy(global_matches);
    ClassTableIteratorDestroy(iter);

    return found;
}

static StringSet *GetTagsFromAugmentsTags(const char *item_type,
                                          const char *key,
                                          const JsonElement *json_tags,
                                          const char *default_tag,
                                          const char *filename)
{
    StringSet *tags = NULL;
    if (JSON_NOT_NULL(json_tags))
    {
        if ((JsonGetType(json_tags) != JSON_TYPE_ARRAY) ||
            (!JsonArrayContainsOnlyPrimitives((JsonElement*) json_tags)))
        {
            Log(LOG_LEVEL_ERR,
                "Invalid tags information for %s '%s' in augments file '%s':"
                " must be a JSON array of strings",
                item_type, key, filename);
        }
        else
        {
            tags = JsonArrayToStringSet(json_tags);
            if (tags == NULL)
            {
                Log(LOG_LEVEL_ERR,
                    "Invalid meta information %s '%s' in augments file '%s':"
                    " must be a JSON array of strings",
                    item_type, key, filename);
            }
        }
    }
    if (tags == NULL)
    {
        tags = StringSetNew();
    }
    StringSetAdd(tags, xstrdup(default_tag));

    return tags;
}

static inline bool CanSetVariable(const EvalContext *ctx, VarRef *var_ref)
{
    assert(var_ref != NULL);

    bool null_ns = false;
    if (var_ref->ns == NULL)
    {
        null_ns = true;
        var_ref->ns = "default";
    }
    StringSet *tags = EvalContextVariableTags(ctx, var_ref);
    bool can_set = ((tags == NULL) || !StringSetContains(tags, CMDB_SOURCE_TAG));
    if (!can_set)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Cannot set variable %s:%s.%s from augments, already defined from host-specific data",
            var_ref->ns, var_ref->scope, var_ref->lval);
    }
    if (null_ns)
    {
        var_ref->ns = NULL;
    }

    return can_set;
}

static inline bool CanSetClass(const EvalContext *ctx, const char *class_spec)
{
    char *ns = NULL;
    char *ns_delim = strchr(class_spec, ':');
    if (ns_delim != NULL)
    {
        ns = xstrndup(class_spec, ns_delim - class_spec);
        class_spec = ns_delim + 1;
    }

    StringSet *tags = EvalContextClassTags(ctx, ns, class_spec);
    bool can_set = ((tags == NULL) || !StringSetContains(tags, CMDB_SOURCE_TAG));
    if (!can_set)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Cannot set class %s:%s from augments, already defined from host-specific data",
            ns, class_spec);
    }

    return can_set;
}

static inline const char *GetAugmentsComment(const char *item_type, const char *identifier,
                                             const char *file_name, const JsonElement *json_object)
{
    assert(JsonGetType(json_object) == JSON_TYPE_OBJECT);

    JsonElement *json_comment = JsonObjectGet(json_object, AUGMENTS_COMMENT_KEY);
    if (NULL_JSON(json_comment))
    {
        return NULL;
    }

    if (JsonGetType(json_comment) != JSON_TYPE_STRING)
    {
        Log(LOG_LEVEL_ERR,
            "Invalid type of the 'comment' field for the '%s' %s in augments data in '%s', must be a string",
            identifier, item_type, file_name);
        return NULL;
    }

    return JsonPrimitiveGetAsString(json_comment);
}

static bool LoadAugmentsData(EvalContext *ctx, const char *filename, const JsonElement* augment)
{
    bool loaded = false;

    if (JsonGetElementType(augment) != JSON_ELEMENT_TYPE_CONTAINER ||
        JsonGetContainerType(augment) != JSON_CONTAINER_TYPE_OBJECT)
    {
        Log(LOG_LEVEL_ERR, "Invalid augments file contents in '%s', must be a JSON object", filename);
    }
    else
    {
        loaded = true;
        Log(LOG_LEVEL_VERBOSE, "Loaded augments file '%s', installing contents", filename);

        JsonIterator iter = JsonIteratorInit(augment);
        const char *key;
        while ((key = JsonIteratorNextKey(&iter)))
        {
            if (!(StringEqual(key, "vars") ||
                  StringEqual(key, "classes") ||
                  StringEqual(key, "inputs") ||
                  StringEqual(key, "augments")))
            {
                Log(LOG_LEVEL_VERBOSE, "Unknown augments key '%s' in file '%s', skipping it",
                    key, filename);
            }
        }

        /* load variables (if any) */
        JsonElement *element = JsonObjectGet(augment, "vars");
        if (JSON_NOT_NULL(element))
        {
            JsonElement* vars = JsonExpandElement(ctx, element);

            if (vars == NULL ||
                JsonGetElementType(vars) != JSON_ELEMENT_TYPE_CONTAINER ||
                JsonGetContainerType(vars) != JSON_CONTAINER_TYPE_OBJECT)
            {
                Log(LOG_LEVEL_ERR, "Invalid augments vars in '%s', must be a JSON object", filename);
                goto vars_cleanup;
            }

            JsonIterator iter = JsonIteratorInit(vars);
            const char *vkey;
            while ((vkey = JsonIteratorNextKey(&iter)))
            {
                VarRef *ref = VarRefParse(vkey);
                if (ref->ns != NULL)
                {
                    if (ref->scope == NULL)
                    {
                        Log(LOG_LEVEL_ERR, "Invalid variable specification in augments data in '%s': '%s'"
                            " (bundle name has to be specified if namespace is specified)", filename, vkey);
                        VarRefDestroy(ref);
                        continue;
                    }
                }
                if (ref->scope == NULL)
                {
                    ref->scope = xstrdup("def");
                }

                JsonElement *data = JsonObjectGet(vars, vkey);
                if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_PRIMITIVE)
                {
                    char *value = JsonPrimitiveToString(data);
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments variable '%s.%s=%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, value, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_DEF, vkey, value, CF_DATA_TYPE_STRING, "source=augments_file");
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments variable '%s=%s' from file '%s'",
                            vkey, value, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePut(ctx, ref, value, CF_DATA_TYPE_STRING, "source=augments_file");
                        }
                    }
                    free(value);
                }
                else if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_CONTAINER &&
                         JsonGetContainerType(data) == JSON_CONTAINER_TYPE_ARRAY &&
                         JsonArrayContainsOnlyPrimitives(data))
                {
                    // map to slist if the data only has primitives
                    Rlist *data_as_rlist = RlistFromContainer(data);
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments slist variable '%s.%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_DEF,
                                                          vkey, data_as_rlist,
                                                          CF_DATA_TYPE_STRING_LIST,
                                                          "source=augments_file");
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments slist variable '%s' from file '%s'",
                            vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePut(ctx, ref, data_as_rlist, CF_DATA_TYPE_STRING_LIST,
                                                   "source=augments_file");
                        }
                    }

                    RlistDestroy(data_as_rlist);
                }
                else // install as a data container
                {
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments data container variable '%s.%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_DEF,
                                                          vkey, data,
                                                          CF_DATA_TYPE_CONTAINER,
                                                          "source=augments_file");
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments data container variable '%s' from file '%s'",
                            vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            EvalContextVariablePut(ctx, ref, data,
                                                   CF_DATA_TYPE_CONTAINER,
                                                   "source=augments_file");
                        }
                    }
                }
                VarRefDestroy(ref);
            }

          vars_cleanup:
            JsonDestroy(vars);
        }

        /* Uses the new format allowing metadata (CFE-3633) */
        element = JsonObjectGet(augment, "variables");
        if (JSON_NOT_NULL(element))
        {
            JsonElement* variables = JsonExpandElement(ctx, element);

            if (variables == NULL || JsonGetType(variables) != JSON_TYPE_OBJECT)
            {
                Log(LOG_LEVEL_ERR, "Invalid augments variables in '%s', must be a JSON object", filename);
                goto variables_cleanup;
            }

            JsonIterator variables_iter = JsonIteratorInit(variables);
            const char *vkey;
            while ((vkey = JsonIteratorNextKey(&variables_iter)))
            {
                VarRef *ref = VarRefParse(vkey);
                if (ref->ns != NULL)
                {
                    if (ref->scope == NULL)
                    {
                        Log(LOG_LEVEL_ERR, "Invalid variable specification in augments data in '%s': '%s'"
                            " (bundle name has to be specified if namespace is specified)", filename, vkey);
                        VarRefDestroy(ref);
                        continue;
                    }
                }
                if (ref->scope == NULL)
                {
                    ref->scope = xstrdup("def");
                }

                const JsonElement *const var_info = JsonObjectGet(variables, vkey);

                const JsonElement *data;
                StringSet *tags;
                const char *comment = NULL;

                if (JsonGetType(var_info) == JSON_TYPE_OBJECT)
                {
                    data = JsonObjectGet(var_info, AUGMENTS_VARIABLES_DATA);

                    if (NULL_JSON(data))
                    {
                        Log(LOG_LEVEL_ERR, "Missing value for the augments variable '%s' in '%s' (value field is required)",
                            vkey, filename);
                        VarRefDestroy(ref);
                        continue;
                    }

                    const JsonElement *json_tags = JsonObjectGet(var_info, AUGMENTS_VARIABLES_TAGS);
                    tags = GetTagsFromAugmentsTags("variable", vkey, json_tags, "source=augments_file", filename);
                    comment = GetAugmentsComment("variable", vkey, filename, var_info);
                }
                else
                {
                    // Just a bare value, like in "vars", no metadata
                    data = var_info;
                    tags = GetTagsFromAugmentsTags("variable", vkey, NULL, "source=augments_file", filename);
                }

                assert(tags != NULL);
                assert(data != NULL);

                bool installed = false;
                if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_PRIMITIVE)
                {
                    char *value = JsonPrimitiveToString(data);
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments variable '%s.%s=%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, value, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutSpecialTagsSetWithComment(ctx, SPECIAL_SCOPE_DEF, vkey, value,
                                                                                        CF_DATA_TYPE_STRING, tags, comment);
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments variable '%s=%s' from file '%s'",
                            vkey, value, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutTagsSetWithComment(ctx, ref, value, CF_DATA_TYPE_STRING,
                                                                                 tags, comment);
                        }
                    }
                    free(value);
                }
                else if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_CONTAINER &&
                         JsonGetContainerType(data) == JSON_CONTAINER_TYPE_ARRAY &&
                         JsonArrayContainsOnlyPrimitives((JsonElement *) data))
                {
                    // map to slist if the data only has primitives
                    Rlist *data_as_rlist = RlistFromContainer(data);
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments slist variable '%s.%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutSpecialTagsSetWithComment(ctx, SPECIAL_SCOPE_DEF,
                                                                                        vkey, data_as_rlist,
                                                                                        CF_DATA_TYPE_STRING_LIST,
                                                                                        tags, comment);
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments slist variable '%s' from file '%s'",
                            vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutTagsSetWithComment(ctx, ref, data_as_rlist,
                                                                                 CF_DATA_TYPE_STRING_LIST,
                                                                                 tags, comment);
                        }
                    }

                    RlistDestroy(data_as_rlist);
                }
                else // install as a data container
                {
                    if ((ref->ns == NULL) && (ref->scope == NULL))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments data container variable '%s.%s' from file '%s'",
                            SpecialScopeToString(SPECIAL_SCOPE_DEF), vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutSpecialTagsSetWithComment(ctx, SPECIAL_SCOPE_DEF,
                                                                                        vkey, data,
                                                                                        CF_DATA_TYPE_CONTAINER,
                                                                                        tags, comment);
                        }
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments data container variable '%s' from file '%s'",
                            vkey, filename);
                        if (CanSetVariable(ctx, ref))
                        {
                            installed = EvalContextVariablePutTagsSetWithComment(ctx, ref, data,
                                                                                 CF_DATA_TYPE_CONTAINER,
                                                                                 tags, comment);
                        }
                    }
                }
                VarRefDestroy(ref);
                if (!installed)
                {
                    /* EvalContextVariablePutTagsSetWithComment() and
                     * EvalContextVariablePutSpecialTagsSetWithComment() take
                     * over tags in case of success. Otherwise we have to
                     * destroy the set. */
                    StringSetDestroy(tags);
                }
            }

          variables_cleanup:
            JsonDestroy(variables);
        }

        /* load classes (if any) */
        element = JsonObjectGet(augment, "classes");
        if (JSON_NOT_NULL(element))
        {
            JsonElement* classes = JsonExpandElement(ctx, element);

            if (JsonGetElementType(classes) != JSON_ELEMENT_TYPE_CONTAINER ||
                JsonGetContainerType(classes) != JSON_CONTAINER_TYPE_OBJECT)
            {
                Log(LOG_LEVEL_ERR, "Invalid augments classes in '%s', must be a JSON object", filename);
                goto classes_cleanup;
            }

            const char default_tags[] = "source=augments_file";
            JsonIterator iter = JsonIteratorInit(classes);
            const char *ckey;
            while ((ckey = JsonIteratorNextKey(&iter)))
            {
                JsonElement *data = JsonObjectGet(classes, ckey);
                if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_PRIMITIVE)
                {
                    char *check = JsonPrimitiveToString(data);
                    // check if class is true
                    if (CheckContextClassmatch(ctx, check))
                    {
                        Log(LOG_LEVEL_VERBOSE, "Installing augments class '%s' (checked '%s') from file '%s'",
                            ckey, check, filename);
                        if (CanSetClass(ctx, ckey))
                        {
                            EvalContextClassPutSoft(ctx, ckey, CONTEXT_SCOPE_NAMESPACE, default_tags);
                        }
                    }
                    free(check);
                }
                else if (JsonGetElementType(data) == JSON_ELEMENT_TYPE_CONTAINER &&
                         JsonGetContainerType(data) == JSON_CONTAINER_TYPE_ARRAY &&
                         JsonArrayContainsOnlyPrimitives(data))
                {
                    // check if each class is true
                    JsonIterator data_iter = JsonIteratorInit(data);
                    const JsonElement *el;
                    while ((el = JsonIteratorNextValueByType(&data_iter, JSON_ELEMENT_TYPE_PRIMITIVE, true)))
                    {
                        char *check = JsonPrimitiveToString(el);
                        if (CheckContextClassmatch(ctx, check))
                        {
                            Log(LOG_LEVEL_VERBOSE, "Installing augments class '%s' (checked array entry '%s') from file '%s'",
                                ckey, check, filename);
                            if (CanSetClass(ctx, ckey))
                            {
                                EvalContextClassPutSoft(ctx, ckey, CONTEXT_SCOPE_NAMESPACE, default_tags);
                            }
                            free(check);
                            break;
                        }

                        free(check);
                    }
                }
                else if (JsonGetType(data) == JSON_TYPE_OBJECT)
                {
                    const JsonElement *class_exprs = JsonObjectGet(data, AUGMENTS_CLASSES_CLASS_EXPRESSIONS);
                    const JsonElement *reg_exprs = JsonObjectGet(data, AUGMENTS_CLASSES_REGULAR_EXPRESSIONS);
                    const JsonElement *json_tags = JsonObjectGet(data, AUGMENTS_CLASSES_TAGS);

                    if ((JSON_NOT_NULL(class_exprs) && JSON_NOT_NULL(reg_exprs)) ||
                        (NULL_JSON(class_exprs) && NULL_JSON(reg_exprs)))
                    {
                        Log(LOG_LEVEL_ERR, "Invalid augments class data for class '%s' in '%s':"
                            " either \"class_expressions\" or \"regular_expressions\" need to be specified",
                            ckey, filename);
                        continue;
                    }

                    StringSet *tags = GetTagsFromAugmentsTags("class", ckey, json_tags,
                                                              "source=augments_file", filename);
                    const char *comment = GetAugmentsComment("class", ckey, filename, data);
                    bool installed = false;
                    JsonIterator exprs_iter = JsonIteratorInit(class_exprs ? class_exprs : reg_exprs);
                    const JsonElement *el;
                    while ((el = JsonIteratorNextValueByType(&exprs_iter, JSON_ELEMENT_TYPE_PRIMITIVE, true)))
                    {
                        char *check = JsonPrimitiveToString(el);
                        if (CheckContextClassmatch(ctx, check))
                        {
                            Log(LOG_LEVEL_VERBOSE, "Installing augments class '%s' (checked array entry '%s') from file '%s'",
                                ckey, check, filename);
                            if (CanSetClass(ctx, ckey))
                            {
                                installed = EvalContextClassPutSoftTagsSetWithComment(ctx, ckey, CONTEXT_SCOPE_NAMESPACE,
                                                                                      tags, comment);
                            }
                            free(check);
                            break;
                        }

                        free(check);
                    }
                    if (!installed)
                    {
                        /* EvalContextClassPutSoftTagsSetWithComment() takes over tags in
                         * case of success. Otherwise we have to destroy the set. */
                        StringSetDestroy(tags);
                    }
                }
                else
                {
                    Log(LOG_LEVEL_ERR, "Invalid augments class data for class '%s' in '%s'",
                        ckey, filename);
                }
            }

          classes_cleanup:
            JsonDestroy(classes);
        }

        /* load inputs (if any) */
        element = JsonObjectGet(augment, "inputs");
        if (JSON_NOT_NULL(element))
        {
            JsonElement* inputs = JsonExpandElement(ctx, element);

            if (JsonGetElementType(inputs) == JSON_ELEMENT_TYPE_CONTAINER &&
                JsonGetContainerType(inputs) == JSON_CONTAINER_TYPE_ARRAY &&
                JsonArrayContainsOnlyPrimitives(inputs))
            {
                Log(LOG_LEVEL_VERBOSE, "Installing augments def.augments_inputs from file '%s'",
                    filename);
                Rlist *rlist = RlistFromContainer(inputs);
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_DEF,
                                              "augments_inputs", rlist,
                                              CF_DATA_TYPE_STRING_LIST,
                                              "source=augments_file");
                RlistDestroy(rlist);
            }
            else
            {
                Log(LOG_LEVEL_ERR, "Trying to augment inputs in '%s' but the value was not a list of strings",
                    filename);
            }

            JsonDestroy(inputs);
        }

        /* load further def.json files (if any) */
        element = JsonObjectGet(augment, "augments");
        if (JSON_NOT_NULL(element))
        {
            JsonElement* further_augments = element;
            assert(further_augments != NULL);

            if (JsonGetElementType(further_augments) == JSON_ELEMENT_TYPE_CONTAINER &&
                JsonGetContainerType(further_augments) == JSON_CONTAINER_TYPE_ARRAY &&
                JsonArrayContainsOnlyPrimitives(further_augments))
            {
                JsonIterator iter = JsonIteratorInit(further_augments);
                const JsonElement *el;
                while ((el = JsonIteratorNextValueByType(&iter, JSON_ELEMENT_TYPE_PRIMITIVE, true)) != NULL)
                {
                    char *nested_filename = JsonPrimitiveToString(el);
                    bool further_loaded = LoadAugmentsFiles(ctx, nested_filename);
                    if (further_loaded)
                    {
                        Log(LOG_LEVEL_VERBOSE, "Completed augmenting from file '%s'", nested_filename);
                    }
                    else
                    {
                        Log(LOG_LEVEL_ERR, "Could not load requested further augments from file '%s'", nested_filename);
                    }
                    free(nested_filename);
                }
            }
            else
            {
                Log(LOG_LEVEL_ERR, "Trying to augment inputs in '%s' but the value was not a list of strings",
                    filename);
            }
        }
    }

    return loaded;
}

static bool LoadAugmentsFiles(EvalContext *ctx, const char *unexpanded_filename)
{
    bool loaded = false;

    char *filename = ExpandScalar(ctx, NULL, "this", unexpanded_filename, NULL);

    if (strstr(filename, "/.json"))
    {
        Log(LOG_LEVEL_DEBUG,
            "Skipping augments file '%s' because it failed to expand the base filename, resulting in '%s'",
            unexpanded_filename, filename);
    }
    else
    {
        Log(LOG_LEVEL_DEBUG, "Searching for augments file '%s' (original '%s')",
            filename, unexpanded_filename);
        if (FileCanOpen(filename, "r"))
        {
            // 5 MB should be enough for most reasonable def.json data
            JsonElement* augment = ReadJsonFile(filename, LOG_LEVEL_ERR, 5 * 1024 * 1024);
            if (augment != NULL)
            {
                loaded = LoadAugmentsData(ctx, filename, augment);
                JsonDestroy(augment);
            }
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "could not load JSON augments from '%s'", filename);
        }
    }

    free(filename);
    return loaded;
}

static bool IsFile(const char *const filename)
{
    struct stat buffer;
    if (stat(filename, &buffer) != 0)
    {
        return false;
    }
    if (S_ISREG(buffer.st_mode) != 0)
    {
        return true;
    }
    return false;
}

void LoadAugments(EvalContext *ctx, GenericAgentConfig *config)
{
    assert(config != NULL);

    char* def_json = NULL;
    // --ignore-preferred-augments command line option:
    if (config->ignore_preferred_augments)
    {
        EvalContextClassPutHard(ctx, "ignore_preferred_augments", "source=command_line_option");
        // def_json is NULL so it will be assigned below
    }
    else
    {
        def_json = StringFormat("%s%c%s", config->input_dir, FILE_SEPARATOR, "def_preferred.json");
        if (!IsFile(def_json))
        {
            // def_preferred.json does not exist or we cannot read it
            FREE_AND_NULL(def_json);
        }
    }

    if (def_json == NULL)
    {
        // No def_preferred.json, either because the feature is disabled
        // or we could not read the file.
        // Fall back to old / default behavior, using def.json:
        def_json = StringFormat("%s%c%s", config->input_dir, FILE_SEPARATOR, "def.json");
    }
    Log(LOG_LEVEL_VERBOSE, "Loading JSON augments from '%s' (input dir '%s', input file '%s'", def_json, config->input_dir, config->input_file);
    LoadAugmentsFiles(ctx, def_json);
    free(def_json);
}

static void AddPolicyEntryVariables (EvalContext *ctx, const GenericAgentConfig *config)
{
    char *abs_input_path = GetAbsolutePath(config->input_file);
    /* both dirname() and basename() may actually modify the string they are given (see man:basename(3)) */
    char *dirname_path = xstrdup(abs_input_path);
    char *basename_path = xstrdup(abs_input_path);
    EvalContextSetEntryPoint(ctx, abs_input_path);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS,
                                  "policy_entry_filename",
                                  abs_input_path,
                                  CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS,
                                  "policy_entry_dirname",
                                  dirname(dirname_path),
                                  CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS,
                                  "policy_entry_basename",
                                  basename(basename_path),
                                  CF_DATA_TYPE_STRING, "source=agent");
    free(abs_input_path);
    free(dirname_path);
    free(basename_path);
}

void GenericAgentDiscoverContext(EvalContext *ctx, GenericAgentConfig *config,
                                 const char *program_name)
{
    assert(config != NULL);

    strcpy(VPREFIX, "");
    if (program_name != NULL)
    {
        strncpy(CF_PROGRAM_NAME, program_name, sizeof(CF_PROGRAM_NAME) - 1);
    }

    Log(LOG_LEVEL_VERBOSE, " %s", NameVersion());
    Banner("Initialization preamble");

    GenericAgentSetDefaultDigest(&CF_DEFAULT_DIGEST, &CF_DEFAULT_DIGEST_LEN);
    GenericAgentInitialize(ctx, config);

    time_t t = SetReferenceTime();
    UpdateTimeClasses(ctx, t);
    SanitizeEnvironment();

    THIS_AGENT_TYPE = config->agent_type;
    LoggingSetAgentType(CF_AGENTTYPES[config->agent_type]);
    EvalContextClassPutHard(ctx, CF_AGENTTYPES[config->agent_type],
                            "cfe_internal,source=agent");

    DetectEnvironment(ctx);
    AddPolicyEntryVariables(ctx, config);

    EvalContextHeapPersistentLoadAll(ctx);
    LoadSystemConstants(ctx);

    const char *bootstrap_arg =
        config->agent_specific.agent.bootstrap_argument;
    const char *bootstrap_ip =
        config->agent_specific.agent.bootstrap_ip;

    /* Are we bootstrapping the agent? */
    if (config->agent_type == AGENT_TYPE_AGENT && bootstrap_arg != NULL)
    {
        EvalContextClassPutHard(ctx, "bootstrap_mode", "report,source=environment");

        if (!config->agent_specific.agent.bootstrap_trigger_policy)
        {
            EvalContextClassPutHard(ctx, "skip_policy_on_bootstrap", "report,source=environment");
        }

        if (!RemoveAllExistingPolicyInInputs(GetInputDir()))
        {
            Log(LOG_LEVEL_ERR,
                "Error removing existing input files prior to bootstrap");
            DoCleanupAndExit(EXIT_FAILURE);
        }

        if (!WriteBuiltinFailsafePolicy(GetInputDir()))
        {
            Log(LOG_LEVEL_ERR,
                "Error writing builtin failsafe to inputs prior to bootstrap");
            DoCleanupAndExit(EXIT_FAILURE);
        }
        GenericAgentConfigSetInputFile(config, GetInputDir(), "failsafe.cf");

        char canonified_ipaddr[strlen(bootstrap_ip) + 1];
        StringCanonify(canonified_ipaddr, bootstrap_ip);

        bool am_policy_server =
            EvalContextClassGet(ctx, NULL, canonified_ipaddr) != NULL;

        if (am_policy_server)
        {
            Log(LOG_LEVEL_INFO, "Assuming role as policy server,"
                " with policy distribution point at: %s", GetMasterDir());
            MarkAsPolicyServer(ctx); // Sets policy_server class
            SetCFEngineRoles(ctx); // Checks policy_server class

            if (!MasterfileExists(GetMasterDir()))
            {
                Log(LOG_LEVEL_ERR, "In order to bootstrap as a policy server,"
                    " the file '%s/promises.cf' must exist.", GetMasterDir());
                DoCleanupAndExit(EXIT_FAILURE);
            }

            CheckAndSetHAState(GetWorkDir(), ctx);
        }
        else
        {
            Log(LOG_LEVEL_INFO, "Assuming role as regular client,"
                " bootstrapping to policy server: %s", bootstrap_arg);
            // Once we have set, or in this case not set,
            // the policy_server class, we can set roles:
            SetCFEngineRoles(ctx);

            if (config->agent_specific.agent.bootstrap_trust_server)
            {
                EvalContextClassPutHard(ctx, "trust_server", "source=agent");
                Log(LOG_LEVEL_NOTICE,
                    "Bootstrap mode: implicitly trusting server, "
                    "use --trust-server=no if server trust is already established");
            }
        }

        WriteAmPolicyHubFile(am_policy_server);

        PolicyServerWriteFile(GetWorkDir(), bootstrap_arg);
        EvalContextSetPolicyServer(ctx, bootstrap_arg);
        char *const bootstrap_id = CreateBootstrapIDFile(GetWorkDir());
        if (bootstrap_id != NULL)
        {
            EvalContextSetBootstrapID(ctx, bootstrap_id);
            free(bootstrap_id);
        }

        /* FIXME: Why it is called here? Can't we move both invocations to before if? */
        UpdateLastPolicyUpdateTime(ctx);
    }
    else
    {
        char *existing_policy_server = PolicyServerReadFile(GetWorkDir());
        if (existing_policy_server)
        {
            Log(LOG_LEVEL_VERBOSE, "This agent is bootstrapped to: %s",
                existing_policy_server);
            EvalContextSetPolicyServer(ctx, existing_policy_server);
            char *const bootstrap_id = ReadBootstrapIDFile(GetWorkDir());
            if (bootstrap_id != NULL)
            {
                EvalContextSetBootstrapID(ctx, bootstrap_id);
                free(bootstrap_id);
            }
            free(existing_policy_server);
            UpdateLastPolicyUpdateTime(ctx);
            if (GetAmPolicyHub())
            {
                // Hub:
                MarkAsPolicyServer(ctx); // Sets policy_server class
                SetCFEngineRoles(ctx); // Checks policy_server class

                /* Should this go in MarkAsPolicyServer() ? */
                CheckAndSetHAState(GetWorkDir(), ctx);
            }
            else
            {
                // Client, set appropriate role:
                SetCFEngineRoles(ctx);
            }
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "This agent is not bootstrapped -"
                " can't find policy_server.dat in: %s", GetWorkDir());
        }
    }

    /* Load CMDB data *before* augments. */
    if (!config->agent_specific.common.no_host_specific && !LoadCMDBData(ctx))
    {
        Log(LOG_LEVEL_ERR, "Failed to load CMDB data");
    }

    if (!config->agent_specific.common.no_augments)
    {
        /* load augments here so that they can make use of the classes added above
         * (especially 'am_policy_hub' and 'policy_server') */
        LoadAugments(ctx, config);
    }
}

static bool IsPolicyPrecheckNeeded(GenericAgentConfig *config, bool force_validation)
{
    bool check_policy = false;

    if (IsFileOutsideDefaultRepository(config->input_file))
    {
        check_policy = true;
        Log(LOG_LEVEL_VERBOSE, "Input file is outside default repository, validating it");
    }
    if (GenericAgentIsPolicyReloadNeeded(config))
    {
        check_policy = true;
        Log(LOG_LEVEL_VERBOSE, "Input file is changed since last validation, validating it");
    }
    if (force_validation)
    {
        check_policy = true;
        Log(LOG_LEVEL_VERBOSE, "always_validate is set, forcing policy validation");
    }

    return check_policy;
}

bool GenericAgentCheckPolicy(GenericAgentConfig *config, bool force_validation, bool write_validated_file)
{
    if (!MissingInputFile(config->input_file))
    {
        {
            if (config->agent_type == AGENT_TYPE_SERVER ||
                config->agent_type == AGENT_TYPE_MONITOR ||
                config->agent_type == AGENT_TYPE_EXECUTOR)
            {
                time_t validated_at = ReadTimestampFromPolicyValidatedFile(config, NULL);
                config->agent_specific.daemon.last_validated_at = validated_at;
            }
        }

        if (IsPolicyPrecheckNeeded(config, force_validation))
        {
            bool policy_check_ok = GenericAgentArePromisesValid(config);
            if (policy_check_ok && write_validated_file)
            {
                GenericAgentTagReleaseDirectory(config,
                                                NULL, // use GetAutotagDir
                                                write_validated_file, // true
                                                GetAmPolicyHub()); // write release ID?
            }

            if (config->agent_specific.agent.bootstrap_argument && !policy_check_ok)
            {
                Log(LOG_LEVEL_VERBOSE, "Policy is not valid, but proceeding with bootstrap");
                return true;
            }

            return policy_check_ok;
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Policy is already validated");
            return true;
        }
    }
    return false;
}

static JsonElement *ReadPolicyValidatedFile(const char *filename)
{
    bool missing = true;
    struct stat sb;
    if (stat(filename, &sb) != -1)
    {
        missing = false;
    }

    JsonElement *validated_doc = ReadJsonFile(filename, LOG_LEVEL_DEBUG, 5 * 1024 * 1024);
    if (validated_doc == NULL)
    {
        Log(missing ? LOG_LEVEL_DEBUG : LOG_LEVEL_VERBOSE, "Could not parse policy_validated JSON file '%s', using dummy data", filename);
        validated_doc = JsonObjectCreate(2);
        if (missing)
        {
            JsonObjectAppendInteger(validated_doc, "timestamp", 0);
        }
        else
        {
            JsonObjectAppendInteger(validated_doc, "timestamp", sb.st_mtime);
        }
    }

    return validated_doc;
}

static JsonElement *ReadPolicyValidatedFileFromMasterfiles(const GenericAgentConfig *config, const char *maybe_dirname)
{
    char filename[CF_MAXVARSIZE];

    GetPromisesValidatedFile(filename, sizeof(filename), config, maybe_dirname);

    return ReadPolicyValidatedFile(filename);
}

/**
 * @brief Writes a file with a contained timestamp to mark a policy file as validated
 * @param filename the filename
 * @return True if successful.
 */
static bool WritePolicyValidatedFile(ARG_UNUSED const GenericAgentConfig *config, const char *filename)
{
    if (!MakeParentDirectory(filename, true, NULL))
    {
        Log(LOG_LEVEL_ERR,
            "Could not write policy validated marker file: %s", filename);
        return false;
    }

    int fd = creat(filename, CF_PERMS_DEFAULT);
    if (fd == -1)
    {
        Log(LOG_LEVEL_ERR, "While writing policy validated marker file '%s', could not create file (create: %s)", filename, GetErrorStr());
        return false;
    }

    JsonElement *info = JsonObjectCreate(3);
    JsonObjectAppendInteger(info, "timestamp", time(NULL));

    Writer *w = FileWriter(fdopen(fd, "w"));
    JsonWrite(w, info, 0);

    WriterClose(w);
    JsonDestroy(info);

    Log(LOG_LEVEL_VERBOSE, "Saved policy validated marker file '%s'", filename);
    return true;
}

/**
 * @brief Writes the policy validation file and release ID to a directory
 * @return True if successful.
 */
bool GenericAgentTagReleaseDirectory(const GenericAgentConfig *config, const char *dirname, bool write_validated, bool write_release)
{
    char local_dirname[PATH_MAX + 1];
    if (dirname == NULL)
    {
        GetAutotagDir(local_dirname, PATH_MAX, NULL);
        dirname = local_dirname;
    }

    char filename[CF_MAXVARSIZE];
    char git_checksum[GENERIC_AGENT_CHECKSUM_SIZE];
    bool have_git_checksum = GeneratePolicyReleaseIDFromGit(git_checksum, sizeof(git_checksum), dirname);

    Log(LOG_LEVEL_DEBUG, "Tagging directory %s for release (write_validated: %s, write_release: %s)",
        dirname,
        write_validated ? "yes" : "no",
        write_release ? "yes" : "no");

    if (write_release)
    {
        // first, tag the release ID
        GetReleaseIdFile(dirname, filename, sizeof(filename));
        char *id = ReadReleaseIdFromReleaseIdFileMasterfiles(dirname);
        if (id == NULL
            || (have_git_checksum &&
                strcmp(id, git_checksum) != 0))
        {
            if (id == NULL)
            {
                Log(LOG_LEVEL_DEBUG, "The release_id of %s was missing", dirname);
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "The release_id of %s needs to be updated", dirname);
            }

            bool wrote_release = WriteReleaseIdFile(filename, dirname);
            if (!wrote_release)
            {
                Log(LOG_LEVEL_VERBOSE, "The release_id file %s was NOT updated", filename);
                free(id);
                return false;
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "The release_id file %s was updated", filename);
            }
        }

        free(id);
    }

    // now, tag the promises_validated
    if (write_validated)
    {
        Log(LOG_LEVEL_DEBUG, "Tagging directory %s for validation", dirname);

        GetPromisesValidatedFile(filename, sizeof(filename), config, dirname);

        bool wrote_validated = WritePolicyValidatedFile(config, filename);

        if (!wrote_validated)
        {
            Log(LOG_LEVEL_VERBOSE, "The promises_validated file %s was NOT updated", filename);
            return false;
        }

        Log(LOG_LEVEL_DEBUG, "The promises_validated file %s was updated", filename);
        return true;
    }

    return true;
}

/**
 * @brief Writes a file with a contained release ID based on git SHA,
 *        or file checksum if git SHA is not available.
 * @param filename the release_id file
 * @param dirname the directory to checksum or get the Git hash
 * @return True if successful
 */
static bool WriteReleaseIdFile(const char *filename, const char *dirname)
{
    char release_id[GENERIC_AGENT_CHECKSUM_SIZE];

    bool have_release_id =
        GeneratePolicyReleaseID(release_id, sizeof(release_id), dirname);

    if (!have_release_id)
    {
        return false;
    }

    int fd = creat(filename, CF_PERMS_DEFAULT);
    if (fd == -1)
    {
        Log(LOG_LEVEL_ERR, "While writing policy release ID file '%s', could not create file (create: %s)", filename, GetErrorStr());
        return false;
    }

    JsonElement *info = JsonObjectCreate(3);
    JsonObjectAppendString(info, "releaseId", release_id);

    Writer *w = FileWriter(fdopen(fd, "w"));
    JsonWrite(w, info, 0);

    WriterClose(w);
    JsonDestroy(info);

    Log(LOG_LEVEL_VERBOSE, "Saved policy release ID file '%s'", filename);
    return true;
}

bool GenericAgentArePromisesValid(const GenericAgentConfig *config)
{
    assert(config != NULL);

    char cmd[CF_BUFSIZE];
    const char* const bindir = GetBinDir();

    Log(LOG_LEVEL_VERBOSE, "Verifying the syntax of the inputs...");
    {
        char cfpromises[CF_MAXVARSIZE];

        snprintf(cfpromises, sizeof(cfpromises), "%s%ccf-promises%s",
                 bindir, FILE_SEPARATOR, EXEC_SUFFIX);

        struct stat sb;
        if (stat(cfpromises, &sb) == -1)
        {
            Log(LOG_LEVEL_ERR,
                "cf-promises%s needs to be installed in %s for pre-validation of full configuration",
                EXEC_SUFFIX, bindir);

            return false;
        }

        if (config->bundlesequence)
        {
            snprintf(cmd, sizeof(cmd), "\"%s\" \"", cfpromises);
        }
        else
        {
            snprintf(cmd, sizeof(cmd), "\"%s\" -c \"", cfpromises);
        }
    }

    strlcat(cmd, config->input_file, CF_BUFSIZE);

    strlcat(cmd, "\"", CF_BUFSIZE);

    if (config->bundlesequence)
    {
        strlcat(cmd, " -b \"", CF_BUFSIZE);
        for (const Rlist *rp = config->bundlesequence; rp; rp = rp->next)
        {
            const char *bundle_ref = RlistScalarValue(rp);
            strlcat(cmd, bundle_ref, CF_BUFSIZE);

            if (rp->next)
            {
                strlcat(cmd, ",", CF_BUFSIZE);
            }
        }
        strlcat(cmd, "\"", CF_BUFSIZE);
    }

    if (config->ignore_preferred_augments)
    {
        strlcat(cmd, " --ignore-preferred-augments", CF_BUFSIZE);
    }

    Log(LOG_LEVEL_VERBOSE, "Checking policy with command '%s'", cmd);

    if (!ShellCommandReturnsZero(cmd, true))
    {
        Log(LOG_LEVEL_ERR, "Policy failed validation with command '%s'", cmd);
        return false;
    }

    return true;
}




/*****************************************************************************/

#if !defined(__MINGW32__)
static void OpenLog(int facility)
{
    openlog(CF_PROGRAM_NAME, LOG_PID | LOG_NOWAIT | LOG_ODELAY, facility);
}
#endif

/*****************************************************************************/

#if !defined(__MINGW32__)
void CloseLog(void)
{
    closelog();
}
#endif

ENTERPRISE_VOID_FUNC_1ARG_DEFINE_STUB(void, GenericAgentAddEditionClasses, EvalContext *, ctx)
{
    EvalContextClassPutHard(ctx, "community_edition", "inventory,attribute_name=none,source=agent");
}

static int GetDefaultLogFacility()
{
    char log_facility_file[PATH_MAX];
    NDEBUG_UNUSED int written = snprintf(log_facility_file, sizeof(log_facility_file) - 1,
                                         "%s%c%s_log_facility.dat", GetStateDir(),
                                         FILE_SEPARATOR, CF_PROGRAM_NAME);
    assert(written < PATH_MAX);
    if (access(log_facility_file, R_OK) != 0)
    {
        return LOG_USER;
    }

    FILE *f = fopen(log_facility_file, "r");
    if (f == NULL)
    {
        return LOG_USER;
    }
    char facility_str[16] = {0}; /* at most "LOG_DAEMON\n" */
    size_t n_read = fread(facility_str, 1, sizeof(facility_str) - 1, f);
    fclose(f);
    if (n_read == 0)
    {
        return LOG_USER;
    }
    if (facility_str[n_read - 1] == '\n')
    {
        facility_str[n_read - 1] = '\0';
    }
    return ParseFacility(facility_str);
}

static bool StoreDefaultLogFacility()
{
    char log_facility_file[PATH_MAX];
    NDEBUG_UNUSED int written = snprintf(log_facility_file, sizeof(log_facility_file) - 1,
                                         "%s%c%s_log_facility.dat", GetStateDir(),
                                         FILE_SEPARATOR, CF_PROGRAM_NAME);
    assert(written < PATH_MAX);

    FILE *f = fopen(log_facility_file, "w");
    if (f == NULL)
    {
        return false;
    }
    const char *facility_str = LogFacilityToString(GetSyslogFacility());
    NDEBUG_UNUSED int printed = fprintf(f, "%s\n", facility_str);
    assert(printed > 0);

    fclose(f);
    return true;
}

void GenericAgentInitialize(EvalContext *ctx, GenericAgentConfig *config)
{
    int force = false;
    struct stat statbuf, sb;
    char vbuff[CF_BUFSIZE];
    char ebuff[CF_EXPANDSIZE];

#ifdef __MINGW32__
    InitializeWindows();
#endif

    /* Set output to line-buffered to avoid truncated debug logs. */

    /* Bug on HP-UX: Buffered output is discarded if you switch buffering mode
       without flushing the buffered output first. This will happen anyway when
       switching modes, so no performance is lost. */
    fflush(stdout);

#ifndef SUNOS_5
    setlinebuf(stdout);
#else
    /* CFE-2527: On Solaris we avoid calling setlinebuf, since fprintf() on
       Solaris 10 and 11 truncates output under certain conditions. We fully
       disable buffering to avoid truncated debug logs; performance impact
       should be minimal because we mostly write full lines anyway. */
    setvbuf(stdout, NULL, _IONBF, 0);
#endif

    DetermineCfenginePort();

    int default_facility = GetDefaultLogFacility();
    OpenLog(default_facility);
    SetSyslogFacility(default_facility);

    EvalContextClassPutHard(ctx, "any", "source=agent");

    GenericAgentAddEditionClasses(ctx); // May set "enterprise_edition" class

    const Class *enterprise_edition = EvalContextClassGet(ctx, "default", "enterprise_edition");
    if (enterprise_edition == NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_edition", "community", CF_DATA_TYPE_STRING, "derived-from=enterprise_edition,report");
    }
    else 
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_edition", "enterprise", CF_DATA_TYPE_STRING, "derived-from=enterprise_edition,report");
    }

    /* Make sure the chroot for recording changes this process would normally
     * make on the system is setup if that was requested. */
    if (ChrootChanges())
    {
        char changes_chroot[PATH_MAX] = {0};
        GetChangesChrootDir(changes_chroot, sizeof(changes_chroot));
        SetChangesChroot(changes_chroot);
        RegisterCleanupFunction(DeleteChangesChroot);
        Log(LOG_LEVEL_WARNING, "All changes in files will be made in the '%s' chroot",
            changes_chroot);
    }

/* Define trusted directories */

    const char *workdir = GetWorkDir();
    const char *bindir = GetBinDir();

    if (!workdir)
    {
        FatalError(ctx, "Error determining working directory");
    }

    Log(LOG_LEVEL_VERBOSE, "Work directory is %s", workdir);

    snprintf(vbuff, CF_BUFSIZE, "%s%cupdate.conf", GetInputDir(), FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%ccf-agent", bindir, FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%coutputs%cspooled_reports", workdir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%clastseen%cintermittencies", workdir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%creports%cvarious", workdir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);

    snprintf(vbuff, CF_BUFSIZE, "%s%c.", GetLogDir(), FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%c.", GetPidDir(), FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);
    snprintf(vbuff, CF_BUFSIZE, "%s%c.", GetStateDir(), FILE_SEPARATOR);
    MakeParentInternalDirectory(vbuff, force, NULL);

    MakeParentInternalDirectory(GetLogDir(), force, NULL);

    snprintf(vbuff, CF_BUFSIZE, "%s", GetInputDir());

    if (stat(vbuff, &sb) == -1)
    {
        FatalError(ctx, " No access to WORKSPACE/inputs dir");
    }

    /* ensure WORKSPACE/inputs directory has all user bits set (u+rwx) */
    if ((sb.st_mode & 0700) != 0700)
    {
        chmod(vbuff, sb.st_mode | 0700);
    }

    snprintf(vbuff, CF_BUFSIZE, "%s%coutputs", workdir, FILE_SEPARATOR);

    if (stat(vbuff, &sb) == -1)
    {
        FatalError(ctx, " No access to WORKSPACE/outputs dir");
    }

    /* ensure WORKSPACE/outputs directory has all user bits set (u+rwx) */
    if ((sb.st_mode & 0700) != 0700)
    {
        chmod(vbuff, sb.st_mode | 0700);
    }

    const char* const statedir = GetStateDir();

    snprintf(ebuff, sizeof(ebuff), "%s%ccf_procs",
             statedir, FILE_SEPARATOR);
    MakeParentDirectory(ebuff, force, NULL);

    if (stat(ebuff, &statbuf) == -1)
    {
        CreateEmptyFile(ebuff);
    }

    snprintf(ebuff, sizeof(ebuff), "%s%ccf_rootprocs",
             statedir, FILE_SEPARATOR);

    if (stat(ebuff, &statbuf) == -1)
    {
        CreateEmptyFile(ebuff);
    }

    snprintf(ebuff, sizeof(ebuff), "%s%ccf_otherprocs",
             statedir, FILE_SEPARATOR);

    if (stat(ebuff, &statbuf) == -1)
    {
        CreateEmptyFile(ebuff);
    }

    snprintf(ebuff, sizeof(ebuff), "%s%cprevious_state%c",
             statedir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentDirectory(ebuff, force, NULL);

    snprintf(ebuff, sizeof(ebuff), "%s%cdiff%c",
             statedir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentDirectory(ebuff, force, NULL);

    snprintf(ebuff, sizeof(ebuff), "%s%cuntracked%c",
             statedir, FILE_SEPARATOR, FILE_SEPARATOR);
    MakeParentDirectory(ebuff, force, NULL);

    OpenNetwork();
    CryptoInitialize();

    CheckWorkingDirectories(ctx);

    /* Initialize keys and networking. cf-key, doesn't need keys. In fact it
       must function properly even without them, so that it generates them! */
    if (config->agent_type != AGENT_TYPE_KEYGEN)
    {
        LoadSecretKeys(NULL, NULL, NULL, NULL);
        char *ipaddr = NULL, *port = NULL;
        PolicyServerLookUpFile(workdir, &ipaddr, &port);
        PolicyHubUpdateKeys(ipaddr);
        free(ipaddr);
        free(port);
    }

    size_t cwd_size = PATH_MAX;
    while (true)
    {
        char cwd[cwd_size];
        if (!getcwd(cwd, cwd_size))
        {
            if (errno == ERANGE)
            {
                cwd_size *= 2;
                continue;
            }
            else
            {
                Log(LOG_LEVEL_WARNING,
                    "Could not determine current directory (getcwd: %s)",
                    GetErrorStr());
                break;
            }
        }

        EvalContextSetLaunchDirectory(ctx, cwd);
        break;
    }

    if (!MINUSF)
    {
        GenericAgentConfigSetInputFile(config, GetInputDir(), "promises.cf");
    }
}

static void GetChangesChrootDir(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/%ju.changes", GetStateDir(), (uintmax_t) getpid());
}

static void DeleteChangesChroot()
{
    char changes_chroot[PATH_MAX] = {0};
    GetChangesChrootDir(changes_chroot, sizeof(changes_chroot));
    Log(LOG_LEVEL_VERBOSE, "Deleting changes chroot '%s'", changes_chroot);
    DeleteDirectoryTree(changes_chroot);

    /* DeleteDirectoryTree() doesn't delete the root of the tree. */
    if (rmdir(changes_chroot) != 0)
    {
        Log(LOG_LEVEL_ERR, "Failed to delete changes chroot '%s'", changes_chroot);
    }
}

void GenericAgentFinalize(EvalContext *ctx, GenericAgentConfig *config)
{
    /* TODO, FIXME: what else from the above do we need to undo here ? */
    if (config->agent_type != AGENT_TYPE_KEYGEN)
    {
        cfnet_shut();
    }
    CryptoDeInitialize();
    GenericAgentConfigDestroy(config);
    EvalContextDestroy(ctx);
}

static bool MissingInputFile(const char *input_file)
{
    struct stat sb;

    if (stat(input_file, &sb) == -1)
    {
        Log(LOG_LEVEL_ERR, "There is no readable input file at '%s'. (stat: %s)", input_file, GetErrorStr());
        return true;
    }

    return false;
}

// Git only.
static bool GeneratePolicyReleaseIDFromGit(char *release_id_out,
#ifdef NDEBUG /* out_size is only used in an assertion */
                                           ARG_UNUSED
#endif
                                           size_t out_size,
                                           const char *policy_dir)
{
    char git_filename[PATH_MAX + 1];
    snprintf(git_filename, PATH_MAX, "%s/.git/HEAD", policy_dir);
    MapName(git_filename);

    // Note: Probably we should not be reading all of these filenames directly,
    // and should instead use git plumbing commands to retrieve the data.
    FILE *git_file = safe_fopen(git_filename, "r");
    if (git_file)
    {
        char git_head[128];
        int scanned = fscanf(git_file, "ref: %127s", git_head);

        if (scanned == 1)
        // Found HEAD Reference which means we are on a checked out branch
        {
            fclose(git_file);
            snprintf(git_filename, PATH_MAX, "%s/.git/%s",
                     policy_dir, git_head);
            git_file = safe_fopen(git_filename, "r");
            Log(LOG_LEVEL_DEBUG, "Found a git HEAD ref");
        }
        else
        {
            Log(LOG_LEVEL_DEBUG,
                "Unable to find HEAD ref in '%s', looking for commit instead",
                git_filename);
            assert(out_size > 40);
            fseek(git_file, 0, SEEK_SET);
            scanned = fscanf(git_file, "%40s", release_id_out);
            fclose(git_file);

            if (scanned == 1)
            {
                Log(LOG_LEVEL_DEBUG,
                    "Found current git checkout pointing to: %s",
                    release_id_out);
                return true;
            }
            else
            {
                /* We didn't find a commit sha in .git/HEAD, so we assume the
                 * git information is invalid. */
                git_file = NULL;
            }
        }
        if (git_file)
        {
            assert(out_size > 40);
            scanned = fscanf(git_file, "%40s", release_id_out);
            fclose(git_file);
            return scanned == 1;
        }
        else
        {
            Log(LOG_LEVEL_DEBUG, "While generating policy release ID, found git head ref '%s', but unable to open (errno: %s)",
                policy_dir, GetErrorStr());
        }
    }
    else
    {
        Log(LOG_LEVEL_DEBUG, "While generating policy release ID, directory is '%s' not a git repository",
            policy_dir);
    }

    return false;
}

static bool GeneratePolicyReleaseIDFromTree(char *release_id_out, size_t out_size,
                                            const char *policy_dir)
{
    if (access(policy_dir, R_OK) != 0)
    {
        Log(LOG_LEVEL_ERR, "Cannot access policy directory '%s' to generate release ID", policy_dir);
        return false;
    }

    // fallback, produce some pseudo sha1 hash
    const EVP_MD *const md = HashDigestFromId(GENERIC_AGENT_CHECKSUM_METHOD);
    if (md == NULL)
    {
        Log(LOG_LEVEL_ERR,
            "Could not determine function for file hashing");
        return false;
    }

    EVP_MD_CTX *crypto_ctx = EVP_MD_CTX_new();
    if (crypto_ctx == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not allocate openssl hash context");
        return false;
    }

    EVP_DigestInit(crypto_ctx, md);

    bool success = HashDirectoryTree(policy_dir,
                                     (const char *[]) { ".cf", ".dat", ".txt", ".conf", ".mustache", ".json", ".yaml", NULL},
                                     crypto_ctx);

    int md_len;
    unsigned char digest[EVP_MAX_MD_SIZE + 1] = { 0 };
    EVP_DigestFinal(crypto_ctx, digest, &md_len);
    EVP_MD_CTX_free(crypto_ctx);

    HashPrintSafe(release_id_out, out_size, digest,
                  GENERIC_AGENT_CHECKSUM_METHOD, false);
    return success;
}

static bool GeneratePolicyReleaseID(char *release_id_out, size_t out_size,
                                    const char *policy_dir)
{
    if (GeneratePolicyReleaseIDFromGit(release_id_out, out_size, policy_dir))
    {
        return true;
    }

    return GeneratePolicyReleaseIDFromTree(release_id_out, out_size,
                                           policy_dir);
}

/**
 * @brief Gets the promises_validated file name depending on context and options
 */
static void GetPromisesValidatedFile(char *filename, size_t max_size, const GenericAgentConfig *config, const char *maybe_dirname)
{
    char dirname[max_size];

    /* TODO overflow error checking! */
    GetAutotagDir(dirname, max_size, maybe_dirname);

    if (maybe_dirname == NULL && MINUSF)
    {
        snprintf(filename, max_size, "%s/validated_%s", dirname, CanonifyName(config->original_input_file));
    }
    else
    {
        snprintf(filename, max_size, "%s/cf_promises_validated", dirname);
    }

    MapName(filename);
}

 /**
 * @brief Gets the promises_validated file name depending on context and options
 */
static void GetAutotagDir(char *dirname, size_t max_size, const char *maybe_dirname)
{
    if (maybe_dirname != NULL)
    {
        strlcpy(dirname, maybe_dirname, max_size);
    }
    else if (MINUSF)
    {
        strlcpy(dirname, GetStateDir(), max_size);
    }
    else
    {
        strlcpy(dirname, GetMasterDir(), max_size);
    }

    MapName(dirname);
}

/**
 * @brief Gets the release_id file name in the given base_path.
 */
void GetReleaseIdFile(const char *base_path, char *filename, size_t max_size)
{
    snprintf(filename, max_size, "%s/cf_promises_release_id", base_path);
    MapName(filename);
}

static JsonElement *ReadReleaseIdFileFromMasterfiles(const char *maybe_dirname)
{
    char filename[CF_MAXVARSIZE];

    GetReleaseIdFile((maybe_dirname == NULL) ? GetMasterDir() : maybe_dirname,
                     filename, sizeof(filename));

    JsonElement *doc = ReadJsonFile(filename, LOG_LEVEL_DEBUG, 5 * 1024 * 1024);
    if (doc == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not parse release_id JSON file %s", filename);
    }

    return doc;
}

static char* ReadReleaseIdFromReleaseIdFileMasterfiles(const char *maybe_dirname)
{
    JsonElement *doc = ReadReleaseIdFileFromMasterfiles(maybe_dirname);
    char *id = NULL;
    if (doc)
    {
        JsonElement *jid = JsonObjectGet(doc, "releaseId");
        if (jid)
        {
            id = xstrdup(JsonPrimitiveGetAsString(jid));
        }
        JsonDestroy(doc);
    }

    return id;
}

// TODO: refactor Read*FromPolicyValidatedMasterfiles
time_t ReadTimestampFromPolicyValidatedFile(const GenericAgentConfig *config, const char *maybe_dirname)
{
    time_t validated_at = 0;
    {
        JsonElement *validated_doc = ReadPolicyValidatedFileFromMasterfiles(config, maybe_dirname);
        if (validated_doc)
        {
            JsonElement *timestamp = JsonObjectGet(validated_doc, "timestamp");
            if (timestamp)
            {
                validated_at = JsonPrimitiveGetAsInteger(timestamp);
            }
            JsonDestroy(validated_doc);
        }
    }

    return validated_at;
}

// TODO: refactor Read*FromPolicyValidatedMasterfiles
char* ReadChecksumFromPolicyValidatedMasterfiles(const GenericAgentConfig *config, const char *maybe_dirname)
{
    char *checksum_str = NULL;

    {
        JsonElement *validated_doc = ReadPolicyValidatedFileFromMasterfiles(config, maybe_dirname);
        if (validated_doc)
        {
            JsonElement *checksum = JsonObjectGet(validated_doc, "checksum");
            if (checksum )
            {
                checksum_str = xstrdup(JsonPrimitiveGetAsString(checksum));
            }
            JsonDestroy(validated_doc);
        }
    }

    return checksum_str;
}

/**
 * @NOTE Updates the config->agent_specific.daemon.last_validated_at timestamp
 *       used by serverd, execd etc daemons when checking for new policies.
 */
bool GenericAgentIsPolicyReloadNeeded(const GenericAgentConfig *config)
{
    time_t validated_at = ReadTimestampFromPolicyValidatedFile(config, NULL);
    time_t now = time(NULL);

    if (validated_at > now)
    {
        Log(LOG_LEVEL_INFO,
            "Clock seems to have jumped back in time, mtime of %jd is newer than current time %jd, touching it",
            (intmax_t) validated_at, (intmax_t) now);

        GenericAgentTagReleaseDirectory(config,
                                        NULL, // use GetAutotagDir
                                        true, // write validated
                                        false); // write release ID
        return true;
    }

    {
        struct stat sb;
        if (stat(config->input_file, &sb) == -1)
        {
            Log(LOG_LEVEL_VERBOSE, "There is no readable input file at '%s'. (stat: %s)", config->input_file, GetErrorStr());
            return true;
        }
        else if (sb.st_mtime > validated_at)
        {
            Log(LOG_LEVEL_VERBOSE, "Input file '%s' has changed since the last policy read attempt (file is newer than previous)", config->input_file);
            return true;
        }
    }

    // Check the directories first for speed and because non-input/data files should trigger an update
    {
        if (IsNewerFileTree( (char *)GetInputDir(), validated_at))
        {
            Log(LOG_LEVEL_VERBOSE, "Quick search detected file changes");
            return true;
        }
    }

    {
        char filename[MAX_FILENAME];
        snprintf(filename, MAX_FILENAME, "%s/policy_server.dat", GetWorkDir());
        MapName(filename);

        struct stat sb;
        if ((stat(filename, &sb) != -1) && (sb.st_mtime > validated_at))
        {
            return true;
        }
    }

    return false;
}

/*******************************************************************/

Seq *ControlBodyConstraints(const Policy *policy, AgentType agent)
{
    for (size_t i = 0; i < SeqLength(policy->bodies); i++)
    {
        const Body *body = SeqAt(policy->bodies, i);

        if (strcmp(body->type, CF_AGENTTYPES[agent]) == 0)
        {
            if (strcmp(body->name, "control") == 0)
            {
                return body->conlist;
            }
        }
    }

    return NULL;
}

/*******************************************************************/

static int ParseFacility(const char *name)
{
    if (strcmp(name, "LOG_USER") == 0)
    {
        return LOG_USER;
    }
    if (strcmp(name, "LOG_DAEMON") == 0)
    {
        return LOG_DAEMON;
    }
    if (strcmp(name, "LOG_LOCAL0") == 0)
    {
        return LOG_LOCAL0;
    }
    if (strcmp(name, "LOG_LOCAL1") == 0)
    {
        return LOG_LOCAL1;
    }
    if (strcmp(name, "LOG_LOCAL2") == 0)
    {
        return LOG_LOCAL2;
    }
    if (strcmp(name, "LOG_LOCAL3") == 0)
    {
        return LOG_LOCAL3;
    }
    if (strcmp(name, "LOG_LOCAL4") == 0)
    {
        return LOG_LOCAL4;
    }
    if (strcmp(name, "LOG_LOCAL5") == 0)
    {
        return LOG_LOCAL5;
    }
    if (strcmp(name, "LOG_LOCAL6") == 0)
    {
        return LOG_LOCAL6;
    }
    if (strcmp(name, "LOG_LOCAL7") == 0)
    {
        return LOG_LOCAL7;
    }
    return -1;
}

static inline const char *LogFacilityToString(int facility)
{
    switch(facility)
    {
        case LOG_LOCAL0: return "LOG_LOCAL0";
        case LOG_LOCAL1: return "LOG_LOCAL1";
        case LOG_LOCAL2: return "LOG_LOCAL2";
        case LOG_LOCAL3: return "LOG_LOCAL3";
        case LOG_LOCAL4: return "LOG_LOCAL4";
        case LOG_LOCAL5: return "LOG_LOCAL5";
        case LOG_LOCAL6: return "LOG_LOCAL6";
        case LOG_LOCAL7: return "LOG_LOCAL7";
        case LOG_USER:   return "LOG_USER";
        case LOG_DAEMON: return "LOG_DAEMON";
        default:         return "UNKNOWN";
    }
}

void SetFacility(const char *retval)
{
    Log(LOG_LEVEL_VERBOSE, "SET Syslog FACILITY = %s", retval);

    CloseLog();
    OpenLog(ParseFacility(retval));
    SetSyslogFacility(ParseFacility(retval));
    if (!StoreDefaultLogFacility())
    {
        Log(LOG_LEVEL_ERR, "Failed to store default log facility");
    }
}

static void CheckWorkingDirectories(EvalContext *ctx)
/* NOTE: We do not care about permissions (ACLs) in windows */
{
    struct stat statbuf;
    char vbuff[CF_BUFSIZE];

    const char* const workdir = GetWorkDir();
    const char* const statedir = GetStateDir();

    if (uname(&VSYSNAME) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't get kernel name info. (uname: %s)", GetErrorStr());
        memset(&VSYSNAME, 0, sizeof(VSYSNAME));
    }

    snprintf(vbuff, CF_BUFSIZE, "%s%c.", workdir, FILE_SEPARATOR);
    MakeParentDirectory(vbuff, false, NULL);

    /* check that GetWorkDir() exists */
    if (stat(GetWorkDir(), &statbuf) == -1)
    {
        FatalError(ctx,"Unable to stat working directory '%s'! (stat: %s)\n",
                   GetWorkDir(), GetErrorStr());
    }

    Log(LOG_LEVEL_VERBOSE, "Making sure that internal directories are private...");

    Log(LOG_LEVEL_VERBOSE, "Checking integrity of the trusted workdir");

    /* fix any improper uid/gid ownership on workdir */
    if (statbuf.st_uid != getuid() || statbuf.st_gid != getgid())
    {
        if (chown(workdir, getuid(), getgid()) == -1)
        {
            const char* error_reason = GetErrorStr();

            Log(LOG_LEVEL_ERR, "Unable to set ownership on '%s' to '%ju.%ju'. (chown: %s)",
                workdir, (uintmax_t)getuid(), (uintmax_t)getgid(), error_reason);
        }
    }

    /* ensure workdir permissions are go-w */
    if ((statbuf.st_mode & 022) != 0)
    {
        if (chmod(workdir, (mode_t) (statbuf.st_mode & ~022)) == -1)
        {
            Log(LOG_LEVEL_ERR, "Unable to set permissions on '%s' to go-w. (chmod: %s)",
                workdir, GetErrorStr());
        }
    }

    MakeParentDirectory(GetStateDir(), false, NULL);
    Log(LOG_LEVEL_VERBOSE, "Checking integrity of the state database");

    snprintf(vbuff, CF_BUFSIZE, "%s", statedir);

    if (stat(vbuff, &statbuf) == -1)
    {
        snprintf(vbuff, CF_BUFSIZE, "%s%c", statedir, FILE_SEPARATOR);
        MakeParentDirectory(vbuff, false, NULL);

        if (chown(vbuff, getuid(), getgid()) == -1)
        {
            Log(LOG_LEVEL_ERR, "Unable to set owner on '%s' to '%ju.%ju'. (chown: %s)", vbuff,
                (uintmax_t)getuid(), (uintmax_t)getgid(), GetErrorStr());
        }

        chmod(vbuff, (mode_t) 0755);
    }
    else
    {
#ifndef __MINGW32__
        if (statbuf.st_mode & 022)
        {
            Log(LOG_LEVEL_ERR, "UNTRUSTED: State directory %s (mode %jo) was not private, world and/or group writeable!", statedir,
                  (uintmax_t)(statbuf.st_mode & 0777));
        }
#endif /* !__MINGW32__ */
    }

    Log(LOG_LEVEL_VERBOSE, "Checking integrity of the module directory");

    snprintf(vbuff, CF_BUFSIZE, "%s%cmodules", workdir, FILE_SEPARATOR);

    if (stat(vbuff, &statbuf) == -1)
    {
        snprintf(vbuff, CF_BUFSIZE, "%s%cmodules%c.", workdir, FILE_SEPARATOR, FILE_SEPARATOR);
        MakeParentDirectory(vbuff, false, NULL);

        if (chown(vbuff, getuid(), getgid()) == -1)
        {
            Log(LOG_LEVEL_ERR, "Unable to set owner on '%s' to '%ju.%ju'. (chown: %s)", vbuff,
                (uintmax_t)getuid(), (uintmax_t)getgid(), GetErrorStr());
        }

        chmod(vbuff, (mode_t) 0700);
    }
    else
    {
#ifndef __MINGW32__
        if (statbuf.st_mode & 022)
        {
            Log(LOG_LEVEL_ERR, "UNTRUSTED: Module directory %s (mode %jo) was not private!", vbuff,
                  (uintmax_t)(statbuf.st_mode & 0777));
        }
#endif /* !__MINGW32__ */
    }

    Log(LOG_LEVEL_VERBOSE, "Checking integrity of the PKI directory");

    snprintf(vbuff, CF_BUFSIZE, "%s%cppkeys", workdir, FILE_SEPARATOR);

    if (stat(vbuff, &statbuf) == -1)
    {
        snprintf(vbuff, CF_BUFSIZE, "%s%cppkeys%c", workdir, FILE_SEPARATOR, FILE_SEPARATOR);
        MakeParentDirectory(vbuff, false, NULL);

        chmod(vbuff, (mode_t) 0700); /* Keys must be immutable to others */
    }
    else
    {
#ifndef __MINGW32__
        if (statbuf.st_mode & 077)
        {
            FatalError(ctx, "UNTRUSTED: Private key directory %s%cppkeys (mode %jo) was not private!\n", workdir,
                       FILE_SEPARATOR, (uintmax_t)(statbuf.st_mode & 0777));
        }
#endif /* !__MINGW32__ */
    }
}


const char *GenericAgentResolveInputPath(const GenericAgentConfig *config, const char *input_file)
{
    static char input_path[CF_BUFSIZE]; /* GLOBAL_R, no initialization needed */

    switch (FilePathGetType(input_file))
    {
    case FILE_PATH_TYPE_ABSOLUTE:
        strlcpy(input_path, input_file, CF_BUFSIZE);
        break;

    case FILE_PATH_TYPE_NON_ANCHORED:
    case FILE_PATH_TYPE_RELATIVE:
        snprintf(input_path, CF_BUFSIZE, "%s%c%s", config->input_dir, FILE_SEPARATOR, input_file);
        break;
    }

    return MapName(input_path);
}

ENTERPRISE_VOID_FUNC_1ARG_DEFINE_STUB(void, GenericAgentWriteVersion, Writer *, w)
{
    WriterWriteF(w, "%s\n", NameVersion());
}

/*******************************************************************/

const char *Version(void)
{
    return VERSION;
}

/*******************************************************************/

const char *NameVersion(void)
{
    return "CFEngine Core " VERSION;
}

/********************************************************************/

static void CleanPidFile(void)
{
    if (unlink(PIDFILE) != 0)
    {
        if (errno != ENOENT)
        {
            Log(LOG_LEVEL_ERR, "Unable to remove pid file '%s'. (unlink: %s)", PIDFILE, GetErrorStr());
        }
    }
}

/********************************************************************/

static void RegisterPidCleanup(void)
{
    RegisterCleanupFunction(&CleanPidFile);
}

/********************************************************************/

void WritePID(char *filename)
{
    pthread_once(&pid_cleanup_once, RegisterPidCleanup);

    snprintf(PIDFILE, CF_BUFSIZE - 1, "%s%c%s", GetPidDir(), FILE_SEPARATOR, filename);

    FILE *fp = safe_fopen_create_perms(PIDFILE, "w", CF_PERMS_DEFAULT);
    if (fp == NULL)
    {
        Log(LOG_LEVEL_INFO, "Could not write to PID file '%s'. (fopen: %s)", filename, GetErrorStr());
        return;
    }

    fprintf(fp, "%ju\n", (uintmax_t)getpid());

    fclose(fp);
}

pid_t ReadPID(char *filename)
{
    char pidfile[PATH_MAX];
    snprintf(pidfile, PATH_MAX - 1, "%s%c%s", GetPidDir(), FILE_SEPARATOR, filename);

    if (access(pidfile, F_OK) != 0)
    {
        Log(LOG_LEVEL_VERBOSE, "PID file '%s' doesn't exist", pidfile);
        return -1;
    }

    FILE *fp = safe_fopen(pidfile, "r");
    if (fp == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not read PID file '%s' (fopen: %s)", filename, GetErrorStr());
        return -1;
    }

    intmax_t pid;
    if (fscanf(fp, "%jd", &pid) != 1)
    {
        Log(LOG_LEVEL_ERR, "Could not read PID from '%s'", pidfile);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return ((pid_t) pid);
}

bool GenericAgentConfigParseArguments(GenericAgentConfig *config, int argc, char **argv)
{
    if (argc == 0)
    {
        return true;
    }

    if (argc > 1)
    {
        return false;
    }

    GenericAgentConfigSetInputFile(config, NULL, argv[0]);
    MINUSF = true;
    return true;
}

bool GenericAgentConfigParseWarningOptions(GenericAgentConfig *config, const char *warning_options)
{
    if (strlen(warning_options) == 0)
    {
        return false;
    }

    if (strcmp("error", warning_options) == 0)
    {
        config->agent_specific.common.parser_warnings_error |= PARSER_WARNING_ALL;
        return true;
    }

    const char *options_start = warning_options;
    bool warnings_as_errors = false;

    if (StringStartsWith(warning_options, "error="))
    {
        options_start = warning_options + strlen("error=");
        warnings_as_errors = true;
    }

    StringSet *warnings_set = StringSetFromString(options_start, ',');
    StringSetIterator it = StringSetIteratorInit(warnings_set);
    const char *warning_str = NULL;
    while ((warning_str = StringSetIteratorNext(&it)))
    {
        int warning = ParserWarningFromString(warning_str);
        if (warning == -1)
        {
            Log(LOG_LEVEL_ERR, "Unrecognized warning '%s'", warning_str);
            StringSetDestroy(warnings_set);
            return false;
        }

        if (warnings_as_errors)
        {
            config->agent_specific.common.parser_warnings_error |= warning;
        }
        else
        {
            config->agent_specific.common.parser_warnings |= warning;
        }
    }

    StringSetDestroy(warnings_set);
    return true;
}

bool GenericAgentConfigParseColor(GenericAgentConfig *config, const char *mode)
{
    if (!mode || strcmp("auto", mode) == 0)
    {
        config->color = config->tty_interactive;
        return true;
    }
    else if (strcmp("always", mode) == 0)
    {
        config->color = true;
        return true;
    }
    else if (strcmp("never", mode) == 0)
    {
        config->color = false;
        return true;
    }
    else
    {
        Log(LOG_LEVEL_ERR, "Unrecognized color mode '%s'", mode);
        return false;
    }
}

bool GetTTYInteractive(void)
{
    return isatty(0) || isatty(1) || isatty(2);
}

GenericAgentConfig *GenericAgentConfigNewDefault(AgentType agent_type, bool tty_interactive)
{
    GenericAgentConfig *config = xmalloc(sizeof(GenericAgentConfig));

    LoggingSetAgentType(CF_AGENTTYPES[agent_type]);
    config->agent_type = agent_type;
    config->tty_interactive = tty_interactive;

    const char *color_env = getenv("CFENGINE_COLOR");
    config->color = (color_env && strcmp(color_env, "1") == 0);

    config->bundlesequence = NULL;

    config->original_input_file = NULL;
    config->input_file = NULL;
    config->input_dir = NULL;
    config->tag_release_dir = NULL;

    config->check_not_writable_by_others = agent_type != AGENT_TYPE_COMMON;
    config->check_runnable = agent_type != AGENT_TYPE_COMMON;
    config->ignore_missing_bundles = false;
    config->ignore_missing_inputs = false;
    config->ignore_preferred_augments = false;

    config->heap_soft = NULL;
    config->heap_negated = NULL;
    config->ignore_locks = false;

    config->protocol_version = CF_PROTOCOL_UNDEFINED;

    config->agent_specific.agent.bootstrap_argument = NULL;
    config->agent_specific.agent.bootstrap_ip = NULL;
    config->agent_specific.agent.bootstrap_port = NULL;
    config->agent_specific.agent.bootstrap_host = NULL;

    /* By default we trust the network when bootstrapping. */
    config->agent_specific.agent.bootstrap_trust_server = true;

    /* By default we run promises.cf as the last step of boostrapping */
    config->agent_specific.agent.bootstrap_trigger_policy = true;

    /* By default we start services during bootstrap */
    config->agent_specific.agent.skip_bootstrap_service_start = false;

    /* Log classes */
    config->agent_specific.agent.report_class_log = false;

    config->agent_specific.common.no_augments = false;
    config->agent_specific.common.no_host_specific = false;

    switch (agent_type)
    {
    case AGENT_TYPE_COMMON:
        config->agent_specific.common.eval_functions = true;
        config->agent_specific.common.show_classes = NULL;
        config->agent_specific.common.show_variables = NULL;
        config->agent_specific.common.policy_output_format = GENERIC_AGENT_CONFIG_COMMON_POLICY_OUTPUT_FORMAT_NONE;
        /* Bitfields of warnings to be recorded, or treated as errors. */
        config->agent_specific.common.parser_warnings = PARSER_WARNING_ALL;
        config->agent_specific.common.parser_warnings_error = 0;
        break;

    case AGENT_TYPE_AGENT:
        config->agent_specific.agent.show_evaluated_classes = NULL;
        config->agent_specific.agent.show_evaluated_variables = NULL;
        break;

    default:
        break;
    }

    return config;
}

void GenericAgentConfigDestroy(GenericAgentConfig *config)
{
    if (config != NULL)
    {
        RlistDestroy(config->bundlesequence);
        StringSetDestroy(config->heap_soft);
        StringSetDestroy(config->heap_negated);
        free(config->original_input_file);
        free(config->input_file);
        free(config->input_dir);
        free(config->tag_release_dir);
        free(config->agent_specific.agent.bootstrap_argument);
        free(config->agent_specific.agent.bootstrap_host);
        free(config->agent_specific.agent.bootstrap_ip);
        free(config->agent_specific.agent.bootstrap_port);
        free(config);
    }
}

void GenericAgentConfigApply(EvalContext *ctx, const GenericAgentConfig *config)
{
    assert(config != NULL);

    EvalContextSetConfig(ctx, config);

    if (config->heap_soft)
    {
        StringSetIterator it = StringSetIteratorInit(config->heap_soft);
        const char *context = NULL;
        while ((context = StringSetIteratorNext(&it)))
        {
            Class *cls = EvalContextClassGet(ctx, NULL, context);
            if (cls && !cls->is_soft)
            {
                FatalError(ctx, "You cannot use -D to define a reserved class");
            }

            EvalContextClassPutSoft(ctx, context, CONTEXT_SCOPE_NAMESPACE, "source=environment");
        }
    }

    if (config->heap_negated != NULL)
    {
        /* Takes ownership of heap_negated. */
        EvalContextSetNegatedClasses(ctx, config->heap_negated);
        ((GenericAgentConfig *)config)->heap_negated = NULL;
    }

    switch (LogGetGlobalLevel())
    {
    case LOG_LEVEL_DEBUG:
        EvalContextClassPutHard(ctx, "debug_mode", "cfe_internal,source=agent");
        EvalContextClassPutHard(ctx, "opt_debug", "cfe_internal,source=agent");
        // fall through
    case LOG_LEVEL_VERBOSE:
        EvalContextClassPutHard(ctx, "verbose_mode", "cfe_internal,source=agent");
        // fall through
    case LOG_LEVEL_INFO:
        EvalContextClassPutHard(ctx, "inform_mode", "cfe_internal,source=agent");
        // fall through
    case LOG_LEVEL_NOTICE:
        EvalContextClassPutHard(ctx, "notice_mode", "cfe_internal,source=agent");
        // fall through
    case LOG_LEVEL_WARNING:
        EvalContextClassPutHard(ctx, "warning_mode", "cfe_internal,source=agent");
        // fall through
    case LOG_LEVEL_ERR:
        EvalContextClassPutHard(ctx, "error_mode", "cfe_internal,source=agent");
        break;
    default:
        break;
    }

    if (config->color)
    {
        LoggingSetColor(config->color);
    }

    if (config->agent_type == AGENT_TYPE_COMMON)
    {
        EvalContextSetEvalOption(ctx, EVAL_OPTION_FULL, false);
        if (config->agent_specific.common.eval_functions)
        {
            EvalContextSetEvalOption(ctx, EVAL_OPTION_EVAL_FUNCTIONS, true);
        }
    }

    EvalContextSetIgnoreLocks(ctx, config->ignore_locks);

    if (DONTDO)
    {
        EvalContextClassPutHard(ctx, "opt_dry_run", "cfe_internal,source=environment");
    }
}

bool CheckAndGenerateFailsafe(const char *inputdir, const char *input_file)
{
    char failsafe_path[CF_BUFSIZE];

    if (strlen(inputdir) + strlen(input_file) > sizeof(failsafe_path) - 2)
    {
        Log(LOG_LEVEL_ERR,
            "Unable to generate path for %s/%s file. Path too long.",
            inputdir, input_file);
        /* We could create dynamically allocated buffer able to hold the
           whole content of the path but this should be unlikely that we
           will end up here. */
        return false;
    }
    snprintf(failsafe_path, CF_BUFSIZE - 1, "%s/%s", inputdir, input_file);
    MapName(failsafe_path);

    if (access(failsafe_path, R_OK) != 0)
    {
        return WriteBuiltinFailsafePolicyToPath(failsafe_path);
    }
    return true;
}

void GenericAgentConfigSetInputFile(GenericAgentConfig *config, const char *inputdir, const char *input_file)
{
    free(config->original_input_file);
    free(config->input_file);
    free(config->input_dir);

    config->original_input_file = xstrdup(input_file);

    if (inputdir && FilePathGetType(input_file) == FILE_PATH_TYPE_NON_ANCHORED)
    {
        config->input_file = StringFormat("%s%c%s", inputdir, FILE_SEPARATOR, input_file);
    }
    else
    {
        config->input_file = xstrdup(input_file);
    }

    config->input_dir = xstrdup(config->input_file);
    if (!ChopLastNode(config->input_dir))
    {
        free(config->input_dir);
        config->input_dir = xstrdup(".");
    }
}

void GenericAgentConfigSetBundleSequence(GenericAgentConfig *config, const Rlist *bundlesequence)
{
    RlistDestroy(config->bundlesequence);
    config->bundlesequence = RlistCopy(bundlesequence);
}

bool GenericAgentPostLoadInit(const EvalContext *ctx)
{
    const char *tls_ciphers =
        EvalContextVariableControlCommonGet(ctx, COMMON_CONTROL_TLS_CIPHERS);
    const char *tls_min_version =
        EvalContextVariableControlCommonGet(ctx, COMMON_CONTROL_TLS_MIN_VERSION);

    const char *system_log_level_str =
        EvalContextVariableControlCommonGet(ctx, COMMON_CONTROL_SYSTEM_LOG_LEVEL);

    LogLevel system_log_level = LogLevelFromString(system_log_level_str);
    if (system_log_level != LOG_LEVEL_NOTHING)
    {
        LogSetGlobalSystemLogLevel(system_log_level);
    }

    return cfnet_init(tls_min_version, tls_ciphers);
}

void SetupSignalsForAgent(void)
{
    signal(SIGINT, HandleSignalsForAgent);
    signal(SIGTERM, HandleSignalsForAgent);
    signal(SIGBUS, HandleSignalsForAgent);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, HandleSignalsForAgent);
    signal(SIGUSR2, HandleSignalsForAgent);
}

void GenericAgentShowContextsFormatted(EvalContext *ctx, const char *regexp)
{
    assert(regexp != NULL);

    ClassTableIterator *iter = EvalContextClassTableIteratorNewGlobal(ctx, NULL, true, true);

    Seq *seq = SeqNew(1000, free);

    Regex *rx = CompileRegex(regexp);

    if (rx == NULL)
    {
        Log(LOG_LEVEL_ERR, "Sorry, we could not compile regular expression %s", regexp);
        return;
    }

    Class *cls = NULL;
    while ((cls = ClassTableIteratorNext(iter)) != NULL)
    {
        char *class_name = ClassRefToString(cls->ns, cls->name);

        if (!RegexPartialMatch(rx, class_name))
        {
            free(class_name);
            continue;
        }

        StringSet *tagset = cls->tags;
        Buffer *tagbuf = StringSetToBuffer(tagset, ',');

        char *line;
        xasprintf(&line, "%-60s %-40s %-40s", class_name, BufferData(tagbuf), NULL_TO_EMPTY_STRING(cls->comment));
        SeqAppend(seq, line);

        BufferDestroy(tagbuf);
        free(class_name);
    }

    RegexDestroy(rx);

    SeqSort(seq, StrCmpWrapper, NULL);

    printf("%-60s %-40s %-40s\n", "Class name", "Meta tags", "Comment");

    for (size_t i = 0; i < SeqLength(seq); i++)
    {
        const char *context = SeqAt(seq, i);
        printf("%s\n", context);
    }

    SeqDestroy(seq);

    ClassTableIteratorDestroy(iter);
}

void GenericAgentShowVariablesFormatted(EvalContext *ctx, const char *regexp)
{
    assert(regexp != NULL);

    VariableTableIterator *iter = EvalContextVariableTableIteratorNew(ctx, NULL, NULL, NULL);
    Variable *v = NULL;

    Seq *seq = SeqNew(2000, free);

    Regex *rx = CompileRegex(regexp);

    if (rx == NULL)
    {
        Log(LOG_LEVEL_ERR, "Sorry, we could not compile regular expression %s", regexp);
        return;
    }

    while ((v = VariableTableIteratorNext(iter)))
    {
        char *varname = VarRefToString(VariableGetRef(v), true);

        if (!RegexPartialMatch(rx, varname))
        {
            free(varname);
            continue;
        }

        Writer *w = StringWriter();

        Rval var_rval = VariableGetRval(v, false);
        if (var_rval.type == RVAL_TYPE_CONTAINER)
        {
            JsonWriteCompact(w, RvalContainerValue(var_rval));
        }
        else
        {
            RvalWrite(w, var_rval);
        }

        const char *var_value;
        if (StringIsPrintable(StringWriterData(w)))
        {
            var_value = StringWriterData(w);
        }
        else
        {
            var_value = "<non-printable>";
        }


        Buffer *tagbuf = NULL;
        StringSet *tagset = VariableGetTags(v);
        if (tagset != NULL)
        {
            tagbuf = StringSetToBuffer(tagset, ',');
        }
        const char *comment = VariableGetComment(v);

        char *line;
        xasprintf(&line, "%-40s %-60s %-40s %-40s",
                  varname, var_value,
                  tagbuf != NULL ? BufferData(tagbuf) : "",
                  NULL_TO_EMPTY_STRING(comment));

        SeqAppend(seq, line);

        BufferDestroy(tagbuf);
        WriterClose(w);
        free(varname);
    }

    RegexDestroy(rx);

    SeqSort(seq, StrCmpWrapper, NULL);

    printf("%-40s %-60s %-40s %-40s\n", "Variable name", "Variable value", "Meta tags", "Comment");

    for (size_t i = 0; i < SeqLength(seq); i++)
    {
        const char *variable = SeqAt(seq, i);
        printf("%s\n", variable);
    }

    SeqDestroy(seq);
    VariableTableIteratorDestroy(iter);
}
