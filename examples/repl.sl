
(def val ())
(def _ ())

(def (print-res obj)
	(cond (!(null? obj)
		(do
			(set! _ obj)
			(print obj)
		)
	))
)

(def (with-unwrap maybe func)
	(if !(empty? maybe)
		(func (unwrap maybe))
		maybe
	)
)

(def (eval-input str)
	(try (eval str))
)

(def (repl)
	(output "repl> ")
	(let ([user-input (input)])
		(cond 
			(!(empty? user-input)
				(set! val (try (parse user-input)))
				(if (empty? val)
					(output "ParseError: Could not parse!\n")
					(do
						(set! val (eval-input (unwrap val)))
						(if (empty? val)
							(output "EvalError: Could not eval!\n")
							(with-unwrap val print-res)
						)
					)
				)
			)
		)
	)
	(repl)
)
(cond 
	((main?)
		(output "---slang REPL---\n")
		(repl)
	)
)
