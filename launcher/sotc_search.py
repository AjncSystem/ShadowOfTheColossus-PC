# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's search: a field at the top of the side bar (Ctrl+F) that finds settings and
pages by their names, their hints and the words players use ("tela preta", "travando",
"controle"). Enter or a click opens the page, scrolls to the setting and marks it for a moment;
settings of the expert pages are found in the simple view too. After the bloodborne_pc
launcher's search (bbport_search.py, github.com/Supermedo/bloodborne_pc, GPL-2.0).

Every row, switch and folder field registers itself while the pages are built (index_entry,
called by the launcher's row/check/folder helpers); the pages themselves are added in
search_ready(). WORDS adds players' words to a setting (by its key) or a page ('page:...').
"""
import unicodedata

import sotc_theme as theme


def _(pt, en):  # replaced by the launcher's translation (sotc_launcher_win)
    return pt

# Players' words for a setting key or a page, in Portuguese and English.
WORDS = {
    'fps_mode': 'fps 30 60 limite travado lento fluido framerate frame rate uncap',
    'framegen': 'geracao quadros frame generation fsr3 dlss-g dlssg f8 interpolacao',
    'fg_hudless': 'hud tremendo tremido letras barra vida stamina shaking',
    'upscaler': 'upscaler dlss fsr bilinear nitidez borrado blurry escala',
    'output_res': 'resolucao resolution 1080p 1440p 4k 720p ultrawide 21:9 tela larga',
    'textures': 'texturas texture qualidade borrada mip streaming maxima alta',
    'af16': 'anisotropico filtro nitido texturas sharp',
    'neo': 'ps4 pro modo pro cinematografico',
    'fullscreen': 'tela cheia fullscreen janela window',
    'present_mode': 'vsync tearing mailbox fifo cortes',
    'vram_use': 'memoria video vram placa gb falta',
    'play_mode': 'modo normal pro extremo personalizado preset qualidade',
    'game_dir': 'jogo pasta dump eboot cusa08809 cusa08034 folder',
    'dlc_dir': 'dlc conteudo adicional',
    'sys_modules': 'som audio mudo sem som ngs2 ult sound',
    'user_dir': 'saves pasta dados backup',
    'exit_backup': 'backup automatico saves fechar queda',
    'language': 'idioma lingua language portugues ingles legenda',
    'volume': 'volume som baixo alto',
    'rumble': 'vibracao vibration controle',
    'button_icons': 'icones botoes xbox playstation',
    'mouse': 'mouse camera rato',
    'skip_logos': 'logos abertura pular intro',
    'fx_motionblur': 'motion blur borrao rastro sombra',
    'check_updates': 'atualizacao update versao',
    'page:saves': 'saves save exportar importar backup jogo salvo',
    'page:controls': 'controle teclado gamepad dualshock dualsense xbox teclas',
    'page:graphics': 'imagem grafico video tela fps',
    'page:log': 'registro log erro queda crash',
    'page:debug': 'debug diagnostico depuracao',
}


def fold(text):
    """Lower case without accents: 'Resolução' -> 'resolucao'."""
    text = unicodedata.normalize('NFKD', str(text).lower())
    return ''.join(c for c in text if not unicodedata.combining(c))


class SearchMixin:
    """Mixed into the Launcher: the index, the field and the jump."""

    def index_entry(self, widget, title, hint='', key=None, page=None):
        page = page or getattr(self, 'building_page', None)
        if not page or not title:
            return
        if not hasattr(self, 'search_index'):
            self.search_index = []
        self.search_index.append({'page': page, 'widget': widget, 'title': str(title).strip(),
                                  'hint': str(hint or ''), 'words': fold(WORDS.get(key or '', '')), 'key': key})

    def search_ready(self, pages):
        """The pages themselves, once built (pages: {name: title})."""
        for name, title in pages.items():
            self.index_entry(None, title, key='page:' + name, page=name)

    def search(self, query, limit=7):
        words = [w for w in fold(query).split() if len(w) > 1 or w.isdigit()]
        if not words:
            return []
        scored = []
        for entry in getattr(self, 'search_index', []):
            title, hint = fold(entry['title']), fold(entry['hint'])
            hits, points = 0, 0
            for w in words:
                if w in title:
                    hits, points = hits + 1, points + (6 if title.startswith(w) else 4)
                elif w in entry['words']:
                    hits, points = hits + 1, points + 3
                elif w in hint:
                    hits, points = hits + 1, points + 1
            if hits:
                scored.append((hits, points, entry['widget'] is None, entry))
        if not scored:
            return []
        best = max(s[0] for s in scored)
        # every word found, or else the entries with the most of them
        scored = [s for s in scored if s[0] == best]
        scored.sort(key=lambda s: (-s[1], s[2]))
        return [s[3] for s in scored[:limit]]

    def build_search(self, side):
        tk, ttk, px = self.tk, self.ttk, self.px
        holder = tk.Frame(side, bg=theme.DEEP)
        holder.pack(fill='x', padx=px(12), pady=(0, px(10)))
        self.search_var = tk.StringVar()
        self.search_entry = ttk.Entry(holder, textvariable=self.search_var, style='Bar.TEntry')
        self.search_entry.pack(fill='x')
        hint = _('Buscar…  Ctrl+F', 'Search…  Ctrl+F')
        glyph = '  ' if theme.HAS_ICONS else ''  # Segoe's magnifier
        self.search_hint = tk.Label(holder, text=glyph + hint, bg=theme.SURFACE, fg=theme.MUTED,
                                    font=(theme.BODY, 9), cursor='xterm')
        self.search_hint.bind('<Button-1>', lambda _e: self.search_entry.focus_set())

        def placeholder(*_a):
            if self.search_var.get() or self.root.focus_get() is self.search_entry:
                self.search_hint.place_forget()
            else:
                self.search_hint.place(x=px(10), rely=0.5, anchor='w')
        self.search_entry.bind('<FocusIn>', placeholder)
        self.search_entry.bind('<FocusOut>', lambda e: (placeholder(), self.root.after(200, self.search_close)))
        self.search_var.trace_add('write', lambda *_a: (placeholder(), self.search_update()))
        self.search_entry.bind('<Down>', lambda _e: self.search_move(1))
        self.search_entry.bind('<Up>', lambda _e: self.search_move(-1))
        self.search_entry.bind('<Return>', lambda _e: self.search_go())
        self.search_entry.bind('<Escape>', lambda _e: (self.search_var.set(''), self.root.focus_set()))
        self.root.bind_all('<Control-f>', self.search_focus)
        self.root.bind_all('<Control-F>', self.search_focus)
        self.root.after(200, placeholder)
        self.search_results, self.search_box, self.search_pick = [], None, 0
        return holder

    def search_focus(self, _event=None):
        self.search_entry.focus_set()
        self.search_entry.select_range(0, 'end')
        return 'break'

    def search_update(self):
        self.search_results = self.search(self.search_var.get())
        self.search_pick = 0
        self.search_show()

    def search_show(self):
        tk, px = self.tk, self.px
        if self.search_box is not None:
            self.search_box.destroy()
            self.search_box = None
        if not self.search_var.get().strip():
            return
        box = tk.Frame(self.root, bg=theme.CARD, highlightthickness=1, highlightbackground=theme.BORDER_STRONG)
        self.search_box = box
        titles = getattr(self, 'page_titles', {})
        if not self.search_results:
            tk.Label(box, text=_('Nada encontrado', 'Nothing found'), bg=theme.CARD, fg=theme.MUTED,
                     font=(theme.BODY, 9), padx=px(12), pady=px(8)).pack(anchor='w')
        for i, entry in enumerate(self.search_results):
            row = tk.Frame(box, bg=theme.CARD_HOVER if i == self.search_pick else theme.CARD, cursor='hand2')
            row.pack(fill='x')
            name = tk.Label(row, text=entry['title'], bg=row['bg'], fg=theme.TEXT_STRONG, anchor='w',
                            font=(theme.BODY_MEDIUM, 10), padx=px(12), pady=(px(2)))
            name.pack(fill='x', pady=(px(4), 0))
            where = tk.Label(row, text=titles.get(entry['page'], entry['page']), bg=row['bg'], fg=theme.GOLD_LIGHT,
                             anchor='w', font=(theme.BODY, 8), padx=px(12))
            where.pack(fill='x', pady=(0, px(4)))
            for widget in (row, name, where):
                widget.bind('<Button-1>', lambda _e, k=i: (setattr(self, 'search_pick', k), self.search_go()))
        side = self.search_entry.master
        x = side.winfo_rootx() - self.root.winfo_rootx()
        y = side.winfo_rooty() - self.root.winfo_rooty() + side.winfo_height() + px(2)
        box.place(x=x, y=y, width=px(340))
        box.lift()

    def search_move(self, step):
        if self.search_results:
            self.search_pick = (self.search_pick + step) % len(self.search_results)
            self.search_show()
        return 'break'

    def search_close(self):
        if self.search_box is not None and self.root.focus_get() is not self.search_entry:
            self.search_box.destroy()
            self.search_box = None

    def search_go(self):
        if not self.search_results:
            return
        entry = self.search_results[self.search_pick]
        self.search_var.set('')
        self.root.focus_set()
        self.jump(entry)

    def jump(self, entry):
        """The entry's page shown (an expert page too), scrolled to the setting, marked a moment."""
        self.show(entry['page'])
        widget = entry['widget']
        if widget is None:
            return
        self.root.update_idletasks()
        page = self.pages.get(entry['page'])
        canvas, inner = getattr(page, 'canvas', None), getattr(page, 'inner', None)
        if canvas is not None and inner is not None:
            y = widget.winfo_rooty() - inner.winfo_rooty()
            height = max(1, inner.winfo_height())
            canvas.yview_moveto(max(0, y - self.px(60)) / height)
        self.flash(widget)

    def flash(self, widget, steps=10):
        """The setting's label in the sword's light, fading back to its colour."""
        try:
            normal = widget.cget('foreground')
        except self.tk.TclError:
            return
        for k in range(steps + 1):
            color = theme.blend(theme.SIGIL_LIGHT, str(normal) or theme.TEXT, k / steps) if k else theme.SIGIL_LIGHT
            self.root.after(1600 + k * 60 if k else 0, lambda c=color: widget.configure(foreground=c)
                            if widget.winfo_exists() else None)
        self.root.after(1600 + (steps + 1) * 60, lambda: widget.configure(foreground=normal)
                        if widget.winfo_exists() else None)
