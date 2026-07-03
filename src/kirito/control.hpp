#ifndef KIRITO_CONTROL_HPP
#define KIRITO_CONTROL_HPP

namespace kirito {

// How statement execution should continue. Break/Continue unwind to the nearest loop; Return
// unwinds to the nearest function call. Normal means "fall through to the next statement".
// Using a signal (not C++ exceptions) keeps the common path exception-free; exceptions are
// reserved for genuine errors.
enum class Flow { Normal, Break, Continue, Return };

}  // namespace kirito

#endif
