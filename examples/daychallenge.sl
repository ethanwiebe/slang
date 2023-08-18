; day challenge

(def (make-date year mon day) (list year mon day))
(def (get-year date) (L date))
(def (get-month date) (L (R date)))
(def (get-day date) (L (R (R date))))

(def test-date (make-date 2001 2 4))

(def (rand-between! low high)
	(+ (% (rand!) (- high low)) low)
)

(def (int->str i)
	(let ((ss (make-str-ostream)))
		(output-to! ss i)
		(stream-get-str ss)
	)
)

(def (sym->str sym)
	(let ((ss (make-str-ostream)))
		(output-to! ss sym)
		(stream-get-str ss)
	)
)

(def (date->str date)
	(let ((ss (make-str-ostream)))
		(output-to! ss 
			(get-year date) "/"
			(zpad2-int (get-month date)) "/"
			(zpad2-int (get-day date))
		)
		(stream-get-str ss)
	)
)

; int -> str
(def (zpad2-int i)
	(let ((ss (make-str-ostream)))
		(if (< i 10)
			(write! ss "0")
		)
		(output-to! ss i)
		(stream-get-str ss)
	)
)

(def (zpad3-int i)
	(let ((ss (make-str-ostream)))
		(if (< i 10)
			(write! ss "0")
		)
		(if (< i 100)
			(write! ss "0")
		)
		(output-to! ss i)
		(stream-get-str ss)
	)
)

(def (output-date date)
	(output (get-year date) "/" (zpad2-int (get-month date)) "/" (zpad2-int (get-day date)))
)

(def (leap-year? year)
	(if !(% year 400)
		true
		(if !(% year 100)
			false
			(if !(% year 4)
				true
				false
			)
		)
	)
)

(def (get-base-year year) (- year 1900))

(def (get-leap-year-count year)
	(let ((base-year (get-base-year (-- year))))
		;(if (< base-year 0)
			;0
			(+ (div base-year 4) -(div base-year 100) (div (+ 300 base-year) 400))
		;)
	)
)

(def month-lengths (vec 31 28 31 30 31 30 31 31 30 31 30 31))
(def month-lengths2 (vec-alloc (len month-lengths) 0))
(let loop ((i 0) (c 0))
	(if (< i (len month-lengths))
		(do
			(vec-set! month-lengths2 i c)
			(loop (++ i) (+ c (vec-get month-lengths i)))
		)
	)
)


; get day index from year 1900
(def (get-day-index date) 
	(let (
			(base-year (get-base-year (get-year date)))
			(year (get-year date))
			(month (-- (get-month date)))
			(day (-- (get-day date)))
		 )
		
		(+ (* 365 base-year) 
			(vec-get month-lengths2 month)
			day
			(get-leap-year-count year)
			(if (and (>= month 2) (leap-year? year))
				1
				0
			)
		)
	)
)

(assert (= 262149 (get-day-index (make-date 2617 9 28))))

(def weekdays (vec 'sun 'mon 'tue 'wed 'thu 'fri 'sat))

(def (get-weekday date)
	(vec-get weekdays
		(% (+ (get-day-index date) 1) 7)
	)
)

(def (get-doomsday year)
	(get-weekday (make-date year 6 6))
)

(def (guess-doomsday year)
	(output "enter doomsday for: " year "\n")
	(let ((s (input)) (dd (get-doomsday year)))
		(cond 
			((= s "q") false)
			((= s (sym->str dd)) (output "correct!\n") true)
			(else (output "answer: " dd "\n") true)
		)
	)
)

(def (guess-day date)
	(output "enter day for: " (date->str date) "\n")
	(let ((s (input)) (w (get-weekday date)))
		(cond
			((= s "q") 'quit)
			((= s (sym->str w)) (output "correct!\n") 'correct)
			(else (output "answer: " w "\n") 'wrong)
		)
	)
)

(def (get-rand-year)
	(rand-between! 0 3000)
)

(def (get-rand-date)
	(let ((year (get-rand-year))
			(month (rand-between! 1 13))
			(day (rand-between! 1 (vec-get month-lengths (-- month))))
		)
		(make-date year month day)
	)
)

(def (doomsday-trainer)
	(output "---doomsday trainer---\n")
	(output "type q to leave\n")
	(let loop ((year (get-rand-year)))
		(case (guess-doomsday year)
			((false))
			(else (loop (get-rand-year)))
		)
	)
)

(def (day-trainer)
	(output "---day trainer---\n")
	(output "type q to leave\n")
	(let loop ((date (get-rand-date)))
		(case (guess-day date)
			((quit))
			(else (loop (get-rand-date)))
		)
	)
)

(def (time-rounded time)
	(/ (floor (+ 0.5 (* time 1000))) 1000.0)
)

(def (get-time-str seconds)
	(let (
			(ss (make-str-ostream))
			(m (floor (/ seconds 60)))
			(s (% (floor seconds) 60))
			(millis (% (floor (+ 0.5 (* 1000.0 seconds))) 1000))
		 )
		(if m
			(output-to! ss m ":" (zpad2-int s) "." (zpad3-int millis))
			(output-to! ss s "." (zpad3-int millis))
		)
		(stream-get-str ss)
	)
)

; pb = 4:53.119s
(def (timed-day-trainer)
	(output "---day trainer time challenge---\n")
	(output "try and see how fast you can determine ten dates!\n")
	(output "press enter to start ")
	(input)
	(let ((start-time
		(let outer (
			    (start-time (perftime!))
			    (index 0)
				(date (get-rand-date))
			 )
			(case (guess-day date)
				((correct) (if (< (++ index) 10)
								(outer start-time (++ index) (get-rand-date)) 
								start-time))
						     ; fifteen second penalty
				(else (outer (- start-time 15.0) index (get-rand-date)))
			)
		))
		(end-time (perftime!))
		(time-diff (- end-time start-time))
		)
		
		(output "you correctly guessed ten dates in: \n\t" (get-time-str time-diff) "s\n")
	)
)

(output "---day trainer---\n")
(output "days: sun mon tue wed thu fri sat\n")
(let loop ((s "h"))
	(case s
		(("d") (doomsday-trainer) (output "quitting doomsday trainer...\n"))
		(("g") (day-trainer) (output "quitting day trainer...\n"))
		(("t") (timed-day-trainer))
	
		(("q"))
		(else (output "d: doomsday trainer\ng: day trainer\nt: time challenge\n"))
	)
	(if !(= s "q")
		(loop (input))
	)
)
