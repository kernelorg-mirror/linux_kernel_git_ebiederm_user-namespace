extern const struct match_table no_tokens[];
extern const struct match_table security_tokens[];

extern int split_options(char *options,
			 const struct match_table *mtable,
			 const struct match_table *ntable,
			 const char ***mopts, const char ***nopts,
			 const char ***unmatched);
extern char *join_options(const char *optv[]);
