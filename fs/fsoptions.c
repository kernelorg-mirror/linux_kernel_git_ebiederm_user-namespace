#include <linux/err.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/fsoptions.h>

const struct match_table no_tokens[] = {
	{ }
};

static char *parse_mnt_opt(char *opt, char *end)
{
	char *p, *equal = NULL;
	int open_quote;

	open_quote = 0;
	for (p = opt; (p < end) && *p; p++) {
		if (equal && (*p == '"'))
			open_quote ^= 1;
		if (open_quote)
			continue;
		if (!equal && (*p == '='))
			equal = p;
		else if (*p == ',') {
			end = p;
			break;
		}
	}
	/* Unbalanced quotes? */
	if (open_quote)
		return ERR_PTR(-EINVAL);

	return end;
}

int split_options(char *options,
		  const struct match_table *mtable,
		  const struct match_table *ntable,
		  const char ***mopts, const char ***nopts,
		  const char ***unmatched)
{
	const char **mvec = NULL, **nvec = NULL, **uvec = NULL;
	size_t mcount = 0, ncount = 0, ucount = 0;
	size_t mindex = 0, nindex = 0, uindex = 0;
	substring_t args[MAX_OPT_ARGS];
	char *opt, *end, *comma;
	int err = -ENOMEM;

	if (!options)
		options = "";
	if (!mtable)
		mtable = no_tokens;
	if (!ntable)
		ntable = no_tokens;

	end = options + strlen(options);
	for (opt = comma = options; comma != end; opt = comma + 1) {
		char saved_comma;
		comma = parse_mnt_opt(opt, end);
		if (IS_ERR(comma))
			return PTR_ERR(comma);

		saved_comma = *comma;
		*comma = '\0';
		if (match_token(opt, mtable, args) != MATCH_FAILURE)
			mcount++;
		else if (match_token(opt, ntable, args) != MATCH_FAILURE)
			ncount++;
		else
			ucount++;
		*comma = saved_comma;
	}

	mvec = kmalloc_array(mcount + 1, sizeof(char *), GFP_KERNEL);
	if (!mvec)
		goto fail;

	nvec = kmalloc_array(ncount + 1, sizeof(char *), GFP_KERNEL);
	if (!nvec)
		goto fail;

	uvec = kmalloc_array(ucount + 1, sizeof(char *), GFP_KERNEL);
	if (!uvec)
		goto fail;

	for (opt = comma = options; comma != end; opt = comma + 1) {
		comma = parse_mnt_opt(opt, end);
		if (IS_ERR(comma))
			goto reparse_fail;

		*comma = '\0';
		if (match_token(opt, mtable, args) != MATCH_FAILURE)
			mvec[mindex++] = opt;
		else if (match_token(opt, ntable, args) != MATCH_FAILURE)
			nvec[nindex++] = opt;
		else
			uvec[uindex++] = opt;
	}
	mvec[mindex] = NULL;
	nvec[nindex] = NULL;
	uvec[uindex] = NULL;

	if (mopts)
		*mopts = mvec;
	if (nopts)
		*nopts = nvec;
	if (unmatched)
		*unmatched = uvec;
	return 0;
reparse_fail:
	WARN_ONCE(true, "Reparse of mount options failed!\n");
	err = PTR_ERR(comma);
fail:
	kfree(mvec);
	kfree(nvec);
	kfree(uvec);
	return err;
}

char *join_options(const char *optv[])
{
	char *flat, *flat_opt;
	const char **opt;
	size_t bytes = 0;

	for (opt = optv; *opt; opt++) {
		size_t len = strlen(*opt);
		/* An extra byte for the comma */
		bytes += len + 1;
	}

	flat_opt = flat = kmalloc(bytes + 1, GFP_KERNEL);
	if (!flat)
		return NULL;

	for (opt = optv; *opt; opt++) {
		size_t len = strlen(*opt);
		if (flat_opt != flat) {
			*flat_opt = ',';
			flat_opt += 1;
		}
		memcpy(flat_opt, *opt, len);
		flat_opt += len;
	}
	*flat_opt = '\0';
	return flat;
}
