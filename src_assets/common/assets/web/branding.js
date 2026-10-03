/**
 * @brief Render the fork's name without rewriting translation keys or upstream URLs.
 * @param {string} message Translated display text.
 * @return {string} Helios-branded display text.
 */
export function brandMessage(message) {
  return message.replace(/https?:\/\/[^\s<>"']+|\bSunshine\b/g, token =>
    token === 'Sunshine' ? 'Helios' : token);
}
